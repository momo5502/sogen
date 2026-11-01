#include "../std_include.hpp"
#include "named_pipe.hpp"
#include "../windows_emulator.hpp"
#include "../io_completion_wait.hpp"
#include <algorithm>
#include <utils/finally.hpp>

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

        std::optional<std::u16string> read_awaited_pipe_name(windows_emulator& win_emu, const io_device_context& context)
        {
            constexpr auto name_offset = static_cast<ULONG>(offsetof(file_pipe_wait_for_buffer, name));
            if (context.input_buffer_length < name_offset)
            {
                return std::nullopt;
            }
            const auto name_length =
                win_emu.memory.read_memory<ULONG>(context.input_buffer + offsetof(file_pipe_wait_for_buffer, name_length));
            if (name_length > context.input_buffer_length - name_offset)
            {
                return std::nullopt;
            }
            std::u16string name(name_length / sizeof(char16_t), u'\0');
            win_emu.memory.read_memory(context.input_buffer + name_offset, name.data(), name.size() * sizeof(char16_t));
            constexpr std::u16string_view directory = u"\\Device\\NamedPipe\\";
            auto pipe_name = std::u16string(directory) + name;
            if (windows_path(pipe_name) == windows_path(directory))
            {
                return std::nullopt;
            }
            return pipe_name;
        }

        std::optional<std::chrono::steady_clock::time_point> read_wait_deadline(windows_emulator& win_emu, const io_device_context& context,
                                                                                const LARGE_INTEGER default_timeout)
        {
            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            const auto timeout_specified =
                win_emu.memory.read_memory<BOOLEAN>(context.input_buffer + offsetof(file_pipe_wait_for_buffer, timeout_specified));
            const auto timeout =
                timeout_specified
                    ? win_emu.memory.read_memory<LARGE_INTEGER>(context.input_buffer + offsetof(file_pipe_wait_for_buffer, timeout))
                    : default_timeout;
            const auto deadline = utils::convert_delay_interval_to_time_point(win_emu.clock(), timeout);
            if (deadline == infinite)
            {
                return std::nullopt;
            }
            return deadline;
        }
    }

    void named_pipe::copy_configuration(const named_pipe& source)
    {
        this->name = source.name;
        this->pipe_type = source.pipe_type;
        this->read_mode = source.read_mode;
        this->completion_mode = source.completion_mode;
        this->max_instances = source.max_instances;
        this->inbound_quota = source.inbound_quota;
        this->outbound_quota = source.outbound_quota;
        this->create_options = source.create_options;
        this->default_timeout = source.default_timeout;
    }

    named_pipe_instances find_named_pipe_instances(process_context& process, const std::u16string_view name, const named_pipe* ignored)
    {
        named_pipe_instances instances{};
        const windows_path path{name};
        for (auto& device : process.devices | std::views::values)
        {
            auto* pipe = device.get_internal_device<named_pipe>();
            if (!pipe || pipe == ignored || !pipe->server_end || windows_path(pipe->name) != path)
            {
                continue;
            }
            instances.any = pipe;
            if (pipe->unopened_peer)
            {
                instances.listening = pipe;
                break;
            }
        }
        return instances;
    }

    void complete_named_pipe_waits(windows_emulator& win_emu, const std::u16string_view name, const NTSTATUS status)
    {
        const windows_path path{name};
        for (auto& device : win_emu.process.devices | std::views::values)
        {
            if (auto* pipe = device.get_internal_device<named_pipe>())
            {
                pipe->complete_waits(win_emu, path, status);
            }
        }
    }

    void named_pipe::complete_waits(windows_emulator& win_emu, const windows_path& pipe_path, const NTSTATUS status)
    {
        for (auto it = this->pending_.begin(); it != this->pending_.end();)
        {
            if (it->kind != operation_kind::wait || windows_path(it->awaited.name) != pipe_path)
            {
                ++it;
                continue;
            }
            this->complete(win_emu, *it, {.status = status}, true);
            release_operation(win_emu.process, *it);
            it = this->pending_.erase(it);
        }
    }

    void named_pipe::complete_waits_if_last_server_end(windows_emulator& win_emu) const
    {
        if (this->server_end && !find_named_pipe_instances(win_emu.process, this->name, this).any)
        {
            complete_named_pipe_waits(win_emu, this->name, STATUS_OBJECT_NAME_NOT_FOUND);
        }
    }

    void named_pipe::complete(windows_emulator& win_emu, const pending_operation& operation, const operation_result result,
                              const bool was_pending)
    {
        const auto& context = operation.context;
        const auto status = result.status;
        this->signaled = true;
        if (operation.kind == operation_kind::read && result.transferred)
        {
            win_emu.memory.write_memory(context.output_buffer, operation.operation->data().data(), result.transferred);
        }
        context.io_status_block.write({.Status = status, .Information = result.transferred});
        write_io_status(context, status);
        if (auto* event = win_emu.process.events.get(context.event))
        {
            event->signaled = true;
        }
        // Windows queues no APC and posts no completion packet for an overlapped request that fails immediately; only its
        // caller learns of the failure.
        if (!was_pending && NT_ERROR(status))
        {
            win_emu.wake_idle_vcpu();
            return;
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

        win_emu.wake_idle_vcpu();
    }

    NTSTATUS named_pipe::start_io(windows_emulator& win_emu, io_device_context context, const bool reading)
    {
        if (!this->endpoint || !context.io_status_block)
        {
            return STATUS_INVALID_HANDLE;
        }
        const auto required_access =
            reading ? (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL) : (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL);
        if (!(context.granted_access & required_access))
        {
            return STATUS_ACCESS_DENIED;
        }
        if (reading && this->read_mode != 0)
        {
            return STATUS_NOT_SUPPORTED;
        }
        if ((context.event.bits && !win_emu.process.events.get(context.event)) ||
            (context.completion_port.bits && !win_emu.process.io_completions.get(context.completion_port)))
        {
            return STATUS_INVALID_HANDLE;
        }
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
        return this->enqueue_operation(win_emu, context, std::move(operation), reading ? operation_kind::read : operation_kind::write);
    }

    NTSTATUS named_pipe::start_listen(windows_emulator& win_emu, io_device_context context)
    {
        if (!this->endpoint || !context.io_status_block)
        {
            return STATUS_INVALID_HANDLE;
        }
        if (!this->unopened_peer)
        {
            return STATUS_PIPE_CONNECTED;
        }
        if (this->completion_mode != 0)
        {
            return STATUS_PIPE_LISTENING;
        }
        return this->enqueue_operation(win_emu, context, nullptr, operation_kind::listen);
    }

    bool named_pipe::is_directory() const
    {
        return !this->endpoint && !this->server_end && windows_path(this->name) == windows_path(u"\\Device\\NamedPipe\\");
    }

    bool named_pipe::is_synchronous() const
    {
        return (this->create_options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;
    }

    NTSTATUS named_pipe::start_wait(windows_emulator& win_emu, io_device_context context)
    {
        awaited_pipe awaited{};
        const auto status = this->prepare_wait(win_emu, context, awaited);
        if (status == STATUS_PENDING)
        {
            return this->enqueue_operation(win_emu, context, nullptr, operation_kind::wait, std::move(awaited));
        }
        if (this->is_synchronous())
        {
            return status;
        }
        return this->enqueue_operation(win_emu, context, nullptr, operation_kind::decided, {}, status);
    }

    NTSTATUS named_pipe::prepare_wait(windows_emulator& win_emu, const io_device_context& context, awaited_pipe& awaited) const
    {
        if (!this->is_directory())
        {
            return STATUS_ILLEGAL_FUNCTION;
        }
        auto awaited_name = read_awaited_pipe_name(win_emu, context);
        if (!awaited_name)
        {
            return STATUS_INVALID_PARAMETER;
        }
        const auto instances = find_named_pipe_instances(win_emu.process, *awaited_name);
        if (instances.listening)
        {
            return STATUS_SUCCESS;
        }
        if (!instances.any)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        awaited = {.name = std::move(*awaited_name), .deadline = read_wait_deadline(win_emu, context, instances.any->default_timeout)};
        return STATUS_PENDING;
    }

    NTSTATUS named_pipe::enqueue_operation(windows_emulator& win_emu, io_device_context context,
                                           std::unique_ptr<utils::host_pipe_operation> operation, const operation_kind kind,
                                           awaited_pipe awaited, const NTSTATUS decided_status)
    {
        context.issuing_thread_id = context.thread().id;
        pending_operation pending{.context = context,
                                  .operation = std::move(operation),
                                  .issuing_thread = win_emu.process.threads.find_handle(&context.thread()),
                                  .kind = kind,
                                  .awaited = std::move(awaited),
                                  .decided_status = decided_status};
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
        bool queued{};
        const auto cleanup = utils::finally([&] {
            if (!queued)
            {
                release_operation(win_emu.process, pending);
            }
        });
        this->signaled = false;
        const auto result =
            pending.operation ? pending.operation->check_completion() : utils::pipe_io_result{.status = utils::pipe_io_status::pending};
        if (result.status != utils::pipe_io_status::pending)
        {
            const auto status = translate_pipe_status(result.status);
            this->complete(win_emu, pending, {.status = status, .transferred = result.transferred}, false);
            return status;
        }
        pending.context.vcpu = nullptr;
        context.io_status_block.write({.Status = STATUS_PENDING, .Information = 0});
        write_io_status(context, STATUS_PENDING);
        if (auto* event = win_emu.process.events.get(context.event))
        {
            event->signaled = false;
        }
        this->pending_.push_back(std::move(pending));
        queued = true;
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

    named_pipe::operation_result named_pipe::poll(windows_emulator& win_emu, const pending_operation& operation) const
    {
        switch (operation.kind)
        {
        case operation_kind::listen:
            return {.status = this->unopened_peer ? STATUS_PENDING : STATUS_SUCCESS};
        case operation_kind::decided:
            return {.status = operation.decided_status};
        case operation_kind::wait: {
            const auto instances = find_named_pipe_instances(win_emu.process, operation.awaited.name);
            if (instances.listening)
            {
                return {.status = STATUS_SUCCESS};
            }
            if (!instances.any)
            {
                return {.status = STATUS_OBJECT_NAME_NOT_FOUND};
            }
            return {.status = operation.awaited.has_expired(win_emu.clock()) ? STATUS_IO_TIMEOUT : STATUS_PENDING};
        }
        default: {
            const auto result = operation.operation->check_completion();
            return {.status = translate_pipe_status(result.status), .transferred = result.transferred};
        }
        }
    }

    void named_pipe::work(windows_emulator& win_emu)
    {
        for (auto it = this->pending_.begin(); it != this->pending_.end();)
        {
            const auto result = this->poll(win_emu, *it);
            if (result.status == STATUS_PENDING)
            {
                ++it;
                continue;
            }
            this->complete(win_emu, *it, result, true);
            release_operation(win_emu.process, *it);
            it = this->pending_.erase(it);
        }
    }

    bool named_pipe::has_pending_io(const uint32_t issuing_thread_id) const
    {
        return std::ranges::any_of(this->pending_, [issuing_thread_id](const auto& operation) {
            return operation.context.issuing_thread_id == issuing_thread_id;
        });
    }

    bool named_pipe::cancel_io(windows_emulator& win_emu, const uint64_t io_status_block, const std::optional<uint32_t> issuing_thread_id)
    {
        bool cancelled{};
        for (auto it = this->pending_.begin(); it != this->pending_.end();)
        {
            if (issuing_thread_id && it->context.issuing_thread_id != *issuing_thread_id)
            {
                ++it;
                continue;
            }
            if (io_status_block && it->context.io_status_block.value() != io_status_block &&
                it->context.wow64_io_status_block.value() != io_status_block)
            {
                ++it;
                continue;
            }
            if (it->operation)
            {
                it->operation->cancel();
            }
            const auto result =
                it->operation ? it->operation->check_completion() : utils::pipe_io_result{.status = utils::pipe_io_status::cancelled};
            if (result.status == utils::pipe_io_status::pending)
            {
                cancelled = true;
                ++it;
                continue;
            }
            this->complete(win_emu, *it, {.status = translate_pipe_status(result.status), .transferred = result.transferred}, true);
            release_operation(win_emu.process, *it);
            it = this->pending_.erase(it);
            cancelled = true;
        }
        return cancelled;
    }

    void named_pipe::release_references(process_context& process)
    {
        for (auto& operation : this->pending_)
        {
            if (operation.operation)
            {
                operation.operation->cancel();
            }
            release_operation(process, operation);
        }
        this->pending_.clear();
    }

    void named_pipe::serialize_object(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->name);
        buffer.write(this->pipe_type);
        buffer.write(this->read_mode);
        buffer.write(this->completion_mode);
        buffer.write(this->max_instances);
        buffer.write(this->inbound_quota);
        buffer.write(this->outbound_quota);
        buffer.write(this->create_options);
        buffer.write(this->server_end);
        buffer.write(this->signaled);
        buffer.write(this->default_timeout);
        for (const auto& pipe : {this->endpoint, this->unopened_peer})
        {
            buffer.write(static_cast<bool>(pipe));
            if (pipe)
            {
                pipe->serialize(buffer);
            }
        }
        buffer.write(static_cast<uint64_t>(this->pending_.size()));
        for (const auto& pending : this->pending_)
        {
            buffer.write(pending.context);
            buffer.write(static_cast<bool>(pending.operation));
            if (pending.operation)
            {
                pending.operation->serialize(buffer);
            }
            buffer.write(pending.issuing_thread);
            buffer.write(pending.kind);
            buffer.write(pending.awaited.name);
            buffer.write_optional(pending.awaited.deadline);
            buffer.write(pending.decided_status);
        }
    }

    void named_pipe::deserialize_object(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->name);
        buffer.read(this->pipe_type);
        buffer.read(this->read_mode);
        buffer.read(this->completion_mode);
        buffer.read(this->max_instances);
        buffer.read(this->inbound_quota);
        buffer.read(this->outbound_quota);
        buffer.read(this->create_options);
        buffer.read(this->server_end);
        buffer.read(this->signaled);
        buffer.read(this->default_timeout);
        for (auto* pipe : {&this->endpoint, &this->unopened_peer})
        {
            *pipe = buffer.read<bool>() ? utils::host_pipe::deserialize(buffer) : nullptr;
        }
        this->pending_.clear();
        const auto count = buffer.read<uint64_t>();
        for (uint64_t i = 0; i < count; ++i)
        {
            pending_operation pending{.context = buffer.read<io_device_context>()};
            if (buffer.read<bool>())
            {
                pending.operation = std::make_unique<utils::host_pipe_operation>(buffer);
            }
            buffer.read(pending.issuing_thread);
            buffer.read(pending.kind);
            buffer.read(pending.awaited.name);
            buffer.read_optional(pending.awaited.deadline);
            buffer.read(pending.decided_status);
            this->pending_.push_back(std::move(pending));
        }
    }
}
