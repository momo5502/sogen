#include "std_include.hpp"
#include "io_completion_wait.hpp"

namespace io_completion_wait
{
    namespace
    {
        bool is_wait_completion_target_signaled(process_context& process, const handle target_object_handle)
        {
            switch (target_object_handle.value.type)
            {
            case handle_types::event: {
                const auto* e = process.events.get(target_object_handle);
                return e && e->signaled;
            }
            case handle_types::thread: {
                const auto* thread = process.threads.get(target_object_handle);
                return thread && thread->is_terminated();
            }

            case handle_types::semaphore: {
                const auto* semaphore = process.semaphores.get(target_object_handle);
                return semaphore && semaphore->current_count > 0;
            }

            case handle_types::mutant: {
                const auto* mutant = process.mutants.get(target_object_handle);
                return mutant && mutant->locked_count == 0;
            }

            case handle_types::timer:
                return false;

            default:
                return false;
            }
        }

        void release_wait_packet_association(process_context& process, wait_completion_packet& wait_packet)
        {
            release_handle_reference(process, wait_packet.io_completion_handle);
            release_handle_reference(process, wait_packet.target_object_handle);
            wait_packet.associated = false;
        }

        void enqueue_wait_packet_completion(process_context& process, io_completion& completion, const handle wait_packet_handle,
                                            const wait_completion_packet& wait_packet)
        {
            io_completion_message message{};
            message.key_context = wait_packet.key_context;
            message.apc_context = wait_packet.apc_context;
            message.io_status_block = wait_packet.io_status_block;
            message.io_status_block.Information = wait_packet.io_status_information;

            if (wait_packet_handle.bits != 0)
            {
                if (const auto retained_wait_packet = process.wait_completion_packets.duplicate(wait_packet_handle))
                {
                    message.wait_packet_handle = *retained_wait_packet;
                }
            }

            completion.enqueue(message);
        }
    }

    bool is_wait_completion_target_type(const handle target_object_handle)
    {
        switch (target_object_handle.value.type)
        {
        case handle_types::event:
        case handle_types::thread:
        case handle_types::semaphore:
        case handle_types::mutant:
        case handle_types::timer:
            return true;
        default:
            return false;
        }
    }

    bool retain_handle_reference(process_context& process, const handle source_handle, handle& retained_handle)
    {
        if (source_handle.bits == 0)
        {
            retained_handle = {};
            return true;
        }

        if (source_handle.value.is_pseudo)
        {
            retained_handle = source_handle;
            return true;
        }

        auto* store = process.get_handle_store(source_handle);
        if (!store)
        {
            return false;
        }

        const auto duplicated = store->duplicate(source_handle);
        if (!duplicated)
        {
            return false;
        }

        retained_handle = *duplicated;
        return true;
    }

    void release_handle_reference(process_context& process, handle& retained_handle)
    {
        if (retained_handle.bits == 0)
        {
            return;
        }

        if (retained_handle.value.is_pseudo)
        {
            retained_handle = {};
            return;
        }

        if (auto* store = process.get_handle_store(retained_handle))
        {
            (void)store->erase(retained_handle);
        }

        retained_handle = {};
    }

    void clear_wait_packet_completion_state(process_context& process, const handle wait_packet_handle)
    {
        if (wait_packet_handle.bits == 0)
        {
            return;
        }

        auto* wait_packet = process.wait_completion_packets.get(wait_packet_handle);
        if (!wait_packet)
        {
            (void)process.wait_completion_packets.erase(wait_packet_handle);
            return;
        }

        release_wait_packet_association(process, *wait_packet);
        wait_packet->queued_completion = false;

        // Each queued completion keeps an extra packet reference.
        (void)process.wait_completion_packets.erase(wait_packet_handle);
    }

    void cleanup_wait_packet_on_close(process_context& process, const handle wait_packet_handle)
    {
        auto* wait_packet = process.wait_completion_packets.get(wait_packet_handle);
        if (!wait_packet)
        {
            return;
        }

        if (wait_packet->queued_completion)
        {
            if (auto* completion = process.io_completions.get(wait_packet->io_completion_handle))
            {
                if (completion->remove_by_wait_packet(wait_packet_handle))
                {
                    (void)process.wait_completion_packets.erase(wait_packet_handle);
                }
            }
        }

        release_wait_packet_association(process, *wait_packet);
        wait_packet->queued_completion = false;
    }

    void materialize_signaled_wait_packets(process_context& process, const handle io_completion_handle)
    {
        auto* completion = process.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return;
        }

        for (auto& [packet_id, wait_packet] : process.wait_completion_packets)
        {
            if (!wait_packet.associated || wait_packet.queued_completion)
            {
                continue;
            }

            if (wait_packet.io_completion_handle != io_completion_handle)
            {
                continue;
            }

            if (!is_wait_completion_target_signaled(process, wait_packet.target_object_handle))
            {
                continue;
            }

            enqueue_wait_packet_completion(process, *completion, process.wait_completion_packets.make_handle(packet_id), wait_packet);
            wait_packet.queued_completion = true;
        }
    }

    bool dequeue_io_completion_message(process_context& process, const handle io_completion_handle, io_completion_message& out_message)
    {
        auto* completion = process.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return false;
        }

        materialize_signaled_wait_packets(process, io_completion_handle);

        if (!completion->dequeue(out_message))
        {
            return false;
        }

        clear_wait_packet_completion_state(process, out_message.wait_packet_handle);
        return true;
    }

    ULONG dequeue_io_completion_entries(process_context& process, const handle io_completion_handle,
                                        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> out_entries,
                                        const ULONG max_count)
    {
        if (max_count == 0 || !out_entries)
        {
            return 0;
        }

        auto* completion = process.io_completions.get(io_completion_handle);
        if (!completion)
        {
            return 0;
        }

        materialize_signaled_wait_packets(process, io_completion_handle);

        ULONG removed = 0;
        for (; removed < max_count; ++removed)
        {
            io_completion_message message{};
            if (!completion->dequeue(message))
            {
                break;
            }

            FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>> entry{};
            entry.KeyContext = message.key_context;
            entry.ApcContext = message.apc_context;
            entry.IoStatusBlock = message.io_status_block;
            out_entries.write(entry, removed);

            clear_wait_packet_completion_state(process, message.wait_packet_handle);
        }

        return removed;
    }
}
