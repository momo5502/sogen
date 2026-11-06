#pragma once

#include "process_context.hpp"

namespace sogen
{

    namespace io_completion_wait
    {
        bool is_wait_completion_target_type(handle target_object_handle);

        bool retain_handle_reference(process_context& process, const emulator_thread* active_thread, handle source_handle,
                                     handle& retained_handle);
        void release_handle_reference(process_context& process, handle& retained_handle);

        void cleanup_wait_packet_on_close(process_context& process, handle wait_packet_handle);
        void clear_wait_packet_completion_state(process_context& process, handle wait_packet_handle);
        void materialize_signaled_wait_packets(windows_emulator& win_emu, handle io_completion_handle);
        // Every scheduler pass: queues the completion of each wait packet whose target is signaled, whatever port it is on.
        void materialize_all_signaled_wait_packets(windows_emulator& win_emu);

        // A thread that found the port empty starts waiting for a packet (KeRemoveQueueEx).
        void add_waiter(io_completion& completion, uint32_t thread_id);
        // Queues a packet the way KeInsertQueue does: while the port's queue is empty, the latest thread still waiting on the
        // port gets it at once, as the result of its wait; otherwise, or with no waiter, it goes to the end of the queue.
        void insert(windows_emulator& win_emu, handle io_completion_handle, io_completion& completion,
                    const io_completion_message& message);

        // What NtRemoveIoCompletionEx and NtWaitForWorkViaWorkerFactory probe before they look at the handle (19041): the
        // entry array, 32 bytes an entry and 8-aligned, for write, and the ULONG for the number of entries removed.
        NTSTATUS probe_completion_entries(memory_manager& memory, uint64_t entries, ULONG count, uint64_t entries_removed);

        bool dequeue_io_completion_message(windows_emulator& win_emu, handle io_completion_handle, io_completion_message& out_message);
        // Takes up to max_count packets off the port, as KeRemoveQueueEx does for an entry array.
        std::vector<io_completion_message> dequeue_io_completion_entries(windows_emulator& win_emu, handle io_completion_handle,
                                                                         ULONG max_count);

        // The copy of taken packets, which the kernel makes under try once they are off the queue (19041). NtRemoveIoCompletion
        // stores KeyContext, ApcContext, then the IO_STATUS_BLOCK as one 16-byte store. IoRemoveIoCompletion stores each entry
        // of an array that way and goes on with the next entry when one faults; NtRemoveIoCompletionEx and
        // NtWaitForWorkViaWorkerFactory then store the count only if none did. Stores before a fault land, the packets are
        // gone either way, and a fault returns STATUS_ACCESS_VIOLATION.
        NTSTATUS write_message(memory_manager& memory, uint64_t key_context, uint64_t apc_context, uint64_t io_status_block,
                               const io_completion_message& message);
        NTSTATUS write_entries(memory_manager& memory, uint64_t entries, uint64_t entries_removed,
                               std::span<const io_completion_message> messages);
    }

} // namespace sogen
