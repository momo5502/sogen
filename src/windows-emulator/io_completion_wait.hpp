#pragma once

#include "process_context.hpp"

namespace sogen
{

    namespace io_completion_wait
    {
        bool is_wait_completion_target_type(handle target_object_handle);

        bool retain_reference(process_context& process, const emulator_thread* active_thread, handle source_handle,
                              object_reference& retained_reference);
        bool retain_reference(process_context& process, const object_reference& source_reference, object_reference& retained_reference);
        void release_reference(process_context& process, object_reference& retained_reference);

        void cleanup_wait_packet_on_close(process_context& process, handle wait_packet_handle);
        void clear_wait_packet_completion_state(process_context& process, object_reference wait_packet_reference);
        void materialize_signaled_wait_packets(windows_emulator& win_emu, const object_reference& io_completion);
        void materialize_signaled_wait_packets(windows_emulator& win_emu, handle io_completion_handle);
        // Every scheduler pass: queues the completion of each wait packet whose target is signaled, whatever port it is on.
        void materialize_all_signaled_wait_packets(windows_emulator& win_emu);

        // A thread that found the port empty starts waiting for a packet (KeRemoveQueueEx).
        void add_waiter(io_completion& completion, uint32_t thread_id);
        // Queues a packet the way KeInsertQueue does: while the port's queue is empty, the latest thread still waiting on the
        // port gets it at once, as the result of its wait; otherwise, or with no waiter, it goes to the end of the queue.
        void insert(windows_emulator& win_emu, const object_reference& io_completion_reference, io_completion& completion,
                    const io_completion_message& message);
        void insert(windows_emulator& win_emu, handle io_completion_handle, io_completion& completion,
                    const io_completion_message& message);

        // What NtRemoveIoCompletionEx and NtWaitForWorkViaWorkerFactory probe before they look at the handle (19041): the
        // entry array, 32 bytes an entry and 8-aligned, for write, and the ULONG for the number of entries removed.
        NTSTATUS probe_completion_entries(memory_manager& memory, const emulator_thread* thread, uint64_t entries, ULONG count,
                                          uint64_t entries_removed);

        // The worker factory whose worker takes the packets, if the thread taking them is one: it is still counted as waiting while the
        // factory's shutdown packet decides whether to be posted again.
        bool dequeue_io_completion_message(windows_emulator& win_emu, const object_reference& io_completion,
                                           io_completion_message& out_message, const object_reference& waiting_worker_factory = {});
        bool dequeue_io_completion_message(windows_emulator& win_emu, handle io_completion_handle, io_completion_message& out_message);
        // Takes up to max_count packets off the port, as KeRemoveQueueEx does for an entry array.
        std::vector<io_completion_message> dequeue_io_completion_entries(windows_emulator& win_emu, const object_reference& io_completion,
                                                                         ULONG max_count,
                                                                         const object_reference& waiting_worker_factory = {});
        std::vector<io_completion_message> dequeue_io_completion_entries(windows_emulator& win_emu, handle io_completion_handle,
                                                                         ULONG max_count);

        // The copy of taken packets, which the kernel makes under try once they are off the queue (19041). NtRemoveIoCompletion
        // stores KeyContext, ApcContext, then the IO_STATUS_BLOCK as one 16-byte store. IoRemoveIoCompletion stores each entry
        // of an array that way and goes on with the next entry when one faults; NtRemoveIoCompletionEx and
        // NtWaitForWorkViaWorkerFactory then store the count only if none did. The stores are writes, so a page the waiter may
        // not write faults before its guard is looked at. Stores before a fault land, the packets are gone either way, and the
        // result is a fault's code: write_message's first, write_entries' last faulting entry's. The waiter makes the copy, so
        // a guard page grows its stack.
        NTSTATUS write_message(memory_manager& memory, const emulator_thread* thread, uint64_t key_context, uint64_t apc_context,
                               uint64_t io_status_block, const io_completion_message& message);
        NTSTATUS write_entries(memory_manager& memory, const emulator_thread* thread, uint64_t entries, uint64_t entries_removed,
                               std::span<const io_completion_message> messages);
        // KeRemoveQueueEx ends a wait that takes no packet (a timeout, a user APC, an alert) with one entry, the wait's status,
        // which IoRemoveIoCompletion counts without copying it. NtRemoveIoCompletionEx stores that count of 1 under try, as it
        // stores the count of every wait that did not fail (19041); the entries stay as they were.
        NTSTATUS write_status_entry_count(memory_manager& memory, const emulator_thread* thread, uint64_t entries_removed);
    }

} // namespace sogen
