#include "../std_include.hpp"
#include "named_pipe.hpp"
#include "../windows_emulator.hpp"
#include "../io_completion_wait.hpp"

namespace sogen
{
    namespace
    {
        NTSTATUS translate_pipe_status(const utils::pipe_io_status status)
        {
            switch (status)
            {
            case utils::pipe_io_status::success:
                return STATUS_SUCCESS;
            case utils::pipe_io_status::pending:
                return STATUS_PENDING;
            case utils::pipe_io_status::broken:
                return STATUS_PIPE_BROKEN;
            case utils::pipe_io_status::cancelled:
                return STATUS_CANCELLED;
            default:
                return STATUS_UNSUCCESSFUL;
            }
        }
    }

    void named_pipe::copy_configuration(const named_pipe& source)
    {
        this->name = source.name;
        this->access = source.access;
        this->pipe_type = source.pipe_type;
        this->read_mode = source.read_mode;
        this->completion_mode = source.completion_mode;
        this->max_instances = source.max_instances;
        this->inbound_quota = source.inbound_quota;
        this->outbound_quota = source.outbound_quota;
        this->create_options = source.create_options;
        this->default_timeout = source.default_timeout;
    }

    void named_pipe::complete(windows_emulator& win_emu, const pending_operation& operation, const utils::pipe_io_result result,
                              const bool was_pending)
    {
        const auto& context = operation.context;
        const auto status = translate_pipe_status(result.status);
        this->signaled = true;
        if (operation.reading && result.transferred)
        {
            win_emu.memory.write_memory(context.output_buffer, operation.operation->data().data(), result.transferred);
        }
        context.io_status_block.write({.Status = status, .Information = result.transferred});
        write_io_status(context, status);
        if (auto* event = win_emu.process.events.get(context.event))
        {
            event->signaled = true;
        }
        if (context.apc_routine)
        {
            if (auto* thread = win_emu.process.threads.get(operation.issuing_thread))
            {
                thread->pending_apcs.push_back({.apc_routine = context.apc_routine,
                                                .apc_argument1 = context.apc_context,
                                                .apc_argument2 = context.wow64_io_status_block ? context.wow64_io_status_block.value()
                                                                                               : context.io_status_block.value(),
                                                .apc_argument3 = 0});
            }
        }
        else if (was_pending || !NT_SUCCESS(status) || !(context.completion_notification_flags & 1))
        {
            io_device::queue_io_completion(win_emu, context);
        }
    }

    NTSTATUS named_pipe::start_io(windows_emulator& win_emu, io_device_context context, const bool reading)
    {
        if (!this->endpoint || !context.io_status_block)
        {
            return STATUS_INVALID_HANDLE;
        }
        const auto required_access =
            reading ? (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL) : (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL);
        if (!(this->access & required_access))
        {
            return STATUS_ACCESS_DENIED;
        }
        this->signaled = false;
        std::unique_ptr<utils::host_pipe_operation> operation;
        if (reading)
        {
            operation = this->endpoint->read(context.output_buffer_length);
        }
        else
        {
            std::string data(context.input_buffer_length, '\0');
            win_emu.memory.read_memory(context.input_buffer, data.data(), data.size());
            operation = this->endpoint->write(data);
        }
        pending_operation pending{context, std::move(operation), win_emu.process.threads.find_handle(&context.thread()), reading};
        const auto result = pending.operation->check_completion();
        if (result.status != utils::pipe_io_status::pending)
        {
            this->complete(win_emu, pending, result, false);
            return translate_pipe_status(result.status);
        }
        std::array<handle, 3> retained_references{};
        const std::array<handle*, 3> references{&pending.context.event, &pending.context.completion_port, &pending.issuing_thread};
        for (size_t i = 0; i < references.size(); ++i)
        {
            if (!io_completion_wait::retain_handle_reference(win_emu.process, context.vcpu->active_thread, *references[i],
                                                             retained_references[i]))
            {
                for (auto& retained : retained_references)
                {
                    io_completion_wait::release_handle_reference(win_emu.process, retained);
                }
                return STATUS_INVALID_HANDLE;
            }
        }
        for (size_t i = 0; i < references.size(); ++i)
        {
            *references[i] = retained_references[i];
        }
        pending.context.vcpu = nullptr;
        context.io_status_block.write({.Status = STATUS_PENDING});
        write_io_status(context, STATUS_PENDING);
        if (auto* event = win_emu.process.events.get(context.event))
        {
            event->signaled = false;
        }
        this->pending_.push_back(std::move(pending));
        if (this->create_options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT))
        {
            context.thread().await_file_io = context.io_status_block.value();
            win_emu.yield_thread(*context.vcpu);
        }
        return STATUS_PENDING;
    }

    void named_pipe::release_operation(process_context& process, pending_operation& operation)
    {
        io_completion_wait::release_handle_reference(process, operation.context.event);
        io_completion_wait::release_handle_reference(process, operation.context.completion_port);
        io_completion_wait::release_handle_reference(process, operation.issuing_thread);
    }

    void named_pipe::work(windows_emulator& win_emu)
    {
        for (auto it = this->pending_.begin(); it != this->pending_.end();)
        {
            const auto result = it->operation->check_completion();
            if (result.status == utils::pipe_io_status::pending)
            {
                ++it;
                continue;
            }
            this->complete(win_emu, *it, result, true);
            this->release_operation(win_emu.process, *it);
            it = this->pending_.erase(it);
        }
    }

    bool named_pipe::cancel_io(windows_emulator& win_emu, const uint64_t io_status_block)
    {
        bool cancelled{};
        for (auto it = this->pending_.begin(); it != this->pending_.end();)
        {
            if (io_status_block && it->context.io_status_block.value() != io_status_block &&
                it->context.wow64_io_status_block.value() != io_status_block)
            {
                ++it;
                continue;
            }
            it->operation->cancel();
            this->complete(win_emu, *it, it->operation->check_completion(), true);
            this->release_operation(win_emu.process, *it);
            it = this->pending_.erase(it);
            cancelled = true;
        }
        return cancelled;
    }

    void named_pipe::release_references(process_context& process)
    {
        for (auto& operation : this->pending_)
        {
            operation.operation->cancel();
            this->release_operation(process, operation);
        }
        this->pending_.clear();
    }

    void named_pipe::serialize_object(utils::buffer_serializer& buffer) const
    {
        if (this->endpoint)
        {
            throw std::runtime_error("Cannot serialize a connected host pipe");
        }
        buffer.write(this->name);
        buffer.write(this->access);
    }

    void named_pipe::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->name);
        buffer.read(this->access);
    }
}
