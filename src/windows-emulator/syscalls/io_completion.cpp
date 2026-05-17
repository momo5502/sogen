#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../io_completion_wait.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    namespace
    {
        void release_wait_packet_association(const syscall_context& c, wait_completion_packet& wait_packet)
        {
            io_completion_wait::release_handle_reference(c.proc, wait_packet.io_completion_handle);
            io_completion_wait::release_handle_reference(c.proc, wait_packet.target_object_handle);
            wait_packet.associated = false;
        }
    }

    NTSTATUS handle_NtCreateIoCompletion(const syscall_context& c, const emulator_object<handle> io_completion_handle,
                                         const ACCESS_MASK /*desired_access*/,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         const ULONG number_of_concurrent_threads)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName != 0)
            {
                name = read_unicode_string(c.emu, attributes.ObjectName);
            }
        }

        if (!name.empty())
        {
            for (auto& entry : c.proc.io_completions)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    io_completion_handle.write(c.proc.io_completions.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        io_completion completion{};
        completion.name = std::move(name);
        completion.number_of_concurrent_threads = number_of_concurrent_threads;

        io_completion_handle.write(c.proc.io_completions.store(std::move(completion)));
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetIoCompletion(const syscall_context& c, const handle io_completion_handle, const emulator_pointer key_context,
                                      const emulator_pointer apc_context, const NTSTATUS io_status,
                                      const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information)
    {
        auto* completion = c.proc.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return STATUS_INVALID_HANDLE;
        }

        io_completion_message message{};
        message.key_context = key_context;
        message.apc_context = apc_context;
        message.io_status_block.Status = io_status;
        message.io_status_block.Information = io_status_information;

        completion->enqueue(message);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetIoCompletionEx(const syscall_context& c, const handle io_completion_handle,
                                        const handle io_completion_packet_handle, const emulator_pointer key_context,
                                        const emulator_pointer apc_context, const NTSTATUS io_status,
                                        const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information)
    {
        auto* completion = c.proc.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return STATUS_INVALID_HANDLE;
        }

        handle wait_packet_handle{};
        if (io_completion_packet_handle.bits != 0)
        {
            auto* wait_packet = c.proc.wait_completion_packets.get(io_completion_packet_handle);
            if (!wait_packet)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (wait_packet->associated || wait_packet->queued_completion)
            {
                return STATUS_INVALID_PARAMETER;
            }

            handle retained_io_completion_handle{};
            if (!io_completion_wait::retain_handle_reference(c.proc, io_completion_handle, retained_io_completion_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            if (!io_completion_wait::retain_handle_reference(c.proc, io_completion_packet_handle, wait_packet_handle))
            {
                io_completion_wait::release_handle_reference(c.proc, retained_io_completion_handle);
                return STATUS_INVALID_HANDLE;
            }

            wait_packet->queued_completion = true;
            wait_packet->associated = false;
            wait_packet->io_completion_handle = retained_io_completion_handle;
            wait_packet->target_object_handle = {};
            wait_packet->key_context = key_context;
            wait_packet->apc_context = apc_context;
            wait_packet->io_status_block.Status = io_status;
            wait_packet->io_status_block.Information = io_status_information;
            wait_packet->io_status_information = io_status_information;
        }

        io_completion_message message{};
        message.key_context = key_context;
        message.apc_context = apc_context;
        message.io_status_block.Status = io_status;
        message.io_status_block.Information = io_status_information;
        message.wait_packet_handle = wait_packet_handle;

        completion->enqueue(message);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtRemoveIoCompletion(const syscall_context& c, const handle io_completion_handle,
                                         const emulator_object<emulator_pointer> key_context,
                                         const emulator_object<emulator_pointer> apc_context,
                                         const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                         const emulator_object<LARGE_INTEGER> timeout)
    {
        auto* completion = c.proc.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return STATUS_INVALID_HANDLE;
        }

        io_completion_message message{};
        if (io_completion_wait::dequeue_io_completion_message(c.proc, io_completion_handle, message))
        {
            key_context.write_if_valid(message.key_context);
            apc_context.write_if_valid(message.apc_context);
            io_status_block.write_if_valid(message.io_status_block);
            return STATUS_SUCCESS;
        }

        if (timeout && timeout.read().QuadPart == 0)
        {
            return STATUS_TIMEOUT;
        }

        auto& t = c.win_emu.current_thread();
        t.await_objects = {};
        t.await_any = false;
        t.await_time = {};
        t.await_msg = {};
        t.waiting_for_alert = false;
        t.await_io_completion = pending_io_completion_wait{};

        auto& wait = *t.await_io_completion;
        wait.type = io_completion_wait_type::remove_single;
        wait.io_completion_handle = io_completion_handle;
        wait.key_context_ptr = key_context.value();
        wait.apc_context_ptr = apc_context.value();
        wait.io_status_block_ptr = io_status_block.value();

        if (timeout)
        {
            wait.timeout = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());

            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            if (wait.timeout.has_value() && wait.timeout.value() != infinite && wait.timeout.value() < c.win_emu.clock().steady_now())
            {
                t.await_io_completion = {};
                return STATUS_TIMEOUT;
            }
        }

        c.win_emu.yield_thread(false);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtRemoveIoCompletionEx(
        const syscall_context& c, const handle io_completion_handle,
        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information, const ULONG count,
        const emulator_object<ULONG> num_entries_removed, const emulator_object<LARGE_INTEGER> timeout, const BOOLEAN alertable)
    {
        if (count == 0 || !io_completion_information)
        {
            return STATUS_INVALID_PARAMETER;
        }

        auto* completion = c.proc.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto removed =
            io_completion_wait::dequeue_io_completion_entries(c.proc, io_completion_handle, io_completion_information, count);
        num_entries_removed.write_if_valid(removed);
        if (removed > 0)
        {
            return STATUS_SUCCESS;
        }

        if (timeout && timeout.read().QuadPart == 0)
        {
            return STATUS_TIMEOUT;
        }

        auto& t = c.win_emu.current_thread();
        t.await_objects = {};
        t.await_any = false;
        t.await_time = {};
        t.await_msg = {};
        t.waiting_for_alert = false;
        t.await_io_completion = pending_io_completion_wait{};

        auto& wait = *t.await_io_completion;
        wait.type = io_completion_wait_type::remove_multiple;
        wait.io_completion_handle = io_completion_handle;
        wait.completion_entries_ptr = io_completion_information.value();
        wait.entries_removed_ptr = num_entries_removed.value();
        wait.max_entries = count;

        if (timeout)
        {
            wait.timeout = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());

            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            if (wait.timeout.has_value() && wait.timeout.value() != infinite && wait.timeout.value() < c.win_emu.clock().steady_now())
            {
                num_entries_removed.write_if_valid(0);
                t.await_io_completion = {};
                return STATUS_TIMEOUT;
            }
        }

        c.win_emu.yield_thread(alertable);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateWaitCompletionPacket(const syscall_context& c, const emulator_object<handle> wait_packet_handle,
                                                 const ACCESS_MASK /*desired_access*/,
                                                 const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName != 0)
            {
                name = read_unicode_string(c.emu, attributes.ObjectName);
            }
        }

        if (!name.empty())
        {
            for (auto& entry : c.proc.wait_completion_packets)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    wait_packet_handle.write(c.proc.wait_completion_packets.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        wait_completion_packet packet{};
        packet.name = std::move(name);
        wait_packet_handle.write(c.proc.wait_completion_packets.store(std::move(packet)));
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAssociateWaitCompletionPacket(const syscall_context& c, const handle wait_completion_packet_handle,
                                                    const handle io_completion_handle, const handle target_object_handle,
                                                    const emulator_pointer key_context, const emulator_pointer apc_context,
                                                    const NTSTATUS io_status,
                                                    const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) io_status_information,
                                                    const emulator_object<BOOLEAN> already_signaled)
    {
        auto* wait_packet = c.proc.wait_completion_packets.get(wait_completion_packet_handle);
        if (!wait_packet)
        {
            return STATUS_INVALID_HANDLE;
        }

        auto* completion = c.proc.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (!io_completion_wait::is_wait_completion_target_type(target_object_handle))
        {
            return STATUS_OBJECT_TYPE_MISMATCH;
        }

        const auto target_exists = [&]() -> bool {
            switch (target_object_handle.value.type)
            {
            case handle_types::event:
                return c.proc.events.get(target_object_handle) != nullptr;
            case handle_types::thread:
                return c.proc.threads.get(target_object_handle) != nullptr;
            case handle_types::semaphore:
                return c.proc.semaphores.get(target_object_handle) != nullptr;
            case handle_types::mutant:
                return c.proc.mutants.get(target_object_handle) != nullptr;
            case handle_types::timer:
                return c.proc.timers.get(target_object_handle) != nullptr;
            default:
                return false;
            }
        }();

        if (!target_exists)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (wait_packet->associated || wait_packet->queued_completion)
        {
            return STATUS_INVALID_PARAMETER;
        }

        handle retained_io_completion_handle{};
        if (!io_completion_wait::retain_handle_reference(c.proc, io_completion_handle, retained_io_completion_handle))
        {
            return STATUS_INVALID_HANDLE;
        }

        handle retained_target_handle{};
        if (!io_completion_wait::retain_handle_reference(c.proc, target_object_handle, retained_target_handle))
        {
            io_completion_wait::release_handle_reference(c.proc, retained_io_completion_handle);
            return STATUS_INVALID_HANDLE;
        }

        wait_packet->io_completion_handle = retained_io_completion_handle;
        wait_packet->target_object_handle = retained_target_handle;
        wait_packet->key_context = key_context;
        wait_packet->apc_context = apc_context;
        wait_packet->io_status_block.Status = io_status;
        wait_packet->io_status_block.Information = io_status_information;
        wait_packet->io_status_information = io_status_information;
        wait_packet->associated = true;

        io_completion_wait::materialize_signaled_wait_packets(c.proc, io_completion_handle);
        already_signaled.write_if_valid(wait_packet->queued_completion ? TRUE : FALSE);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCancelWaitCompletionPacket(const syscall_context& c, const handle wait_completion_packet_handle,
                                                 const BOOLEAN remove_signaled_packet)
    {
        auto* wait_packet = c.proc.wait_completion_packets.get(wait_completion_packet_handle);
        if (!wait_packet)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (remove_signaled_packet && wait_packet->queued_completion)
        {
            if (auto* completion = c.proc.io_completions.get(wait_packet->io_completion_handle))
            {
                if (completion->remove_by_wait_packet(wait_completion_packet_handle))
                {
                    (void)c.proc.wait_completion_packets.erase(wait_completion_packet_handle);
                }
            }
        }

        release_wait_packet_association(c, *wait_packet);
        wait_packet->queued_completion = false;
        return STATUS_SUCCESS;
    }
}
