#include "std_include.hpp"
#include "io_completion_wait.hpp"
#include "windows_emulator.hpp"
#include "worker_factory_support.hpp"
#include "syscall_utils.hpp"

namespace sogen
{
    namespace io_completion_wait
    {
        namespace
        {
            bool is_wait_completion_target_signaled(windows_emulator& win_emu, const object_reference& target)
            {
                auto& process = win_emu.process;
                const auto type = target.object_type();

                if (target.is_pseudo())
                {
                    if (type == handle_types::process)
                    {
                        return target.pseudo == GUEST_PROCESS_HANDLE && process.exit_status.has_value();
                    }

                    if (type == handle_types::file)
                    {
                        return target.pseudo == STDIN_HANDLE ? win_emu.console().input_available() : true;
                    }

                    return false;
                }

                switch (type)
                {
                case handle_types::process: {
                    const auto* entry = process.processes.get_by_index(target.index);
                    if (!entry || !entry->process || !win_emu.processes())
                    {
                        return false;
                    }
                    const auto status = win_emu.processes()->exit_status(entry->process);
                    return status && status.status.has_value();
                }
                case handle_types::managed_thread: {
                    const auto* entry = process.managed_threads.get_by_index(target.index);
                    if (!entry || !win_emu.processes())
                    {
                        return false;
                    }
                    const auto status = win_emu.processes()->exit_status(entry->process);
                    return status && status.status.has_value();
                }

                case handle_types::event: {
                    const auto* e = process.events.get_by_index(target.index);
                    return e && e->signaled;
                }
                case handle_types::thread: {
                    const auto* thread = process.threads.get_by_index(target.index);
                    return thread && thread->is_terminated();
                }

                case handle_types::semaphore: {
                    const auto* semaphore = process.semaphores.get_by_index(target.index);
                    return semaphore && semaphore->current_count > 0;
                }

                case handle_types::port: {
                    const auto* port = process.ports.get_by_index(target.index);
                    return port != nullptr && port->has_pending_reply();
                }

                case handle_types::mutant: {
                    const auto* mutant = process.mutants.get_by_index(target.index);
                    return mutant && mutant->locked_count == 0;
                }

                case handle_types::timer: {
                    auto* entry = process.timers.get_by_index(target.index);
                    return entry && entry->is_signaled(win_emu.clock());
                }

                default:
                    return false;
                }
            }

            // Delivering a wait-completion-packet completion is a satisfied wait, so it must consume the
            // target's signal exactly like NtWaitForSingleObject would: an auto-reset event resets and a
            // semaphore decrements. Without this, an edge-triggered target stays signaled and floods the
            // completion port with spurious packets (e.g. a games's async scheduler busy-spins forever).
            void consume_wait_completion_target(process_context& process, const object_reference& target)
            {
                if (target.is_pseudo())
                {
                    return;
                }

                switch (target.object_type())
                {
                case handle_types::event: {
                    auto* e = process.events.get_by_index(target.index);
                    if (e && e->type == SynchronizationEvent)
                    {
                        e->signaled = false;
                    }
                    break;
                }

                case handle_types::semaphore: {
                    auto* semaphore = process.semaphores.get_by_index(target.index);
                    if (semaphore)
                    {
                        (void)semaphore->try_lock();
                    }
                    break;
                }

                case handle_types::timer: {
                    if (auto* entry = process.timers.get_by_index(target.index))
                    {
                        entry->consume_signal();
                    }
                    break;
                }

                default:
                    // process/thread are terminal; ports and mutants are left
                    // untouched (a wait packet does not take mutant ownership).
                    break;
                }
            }

            // Every scheduler pass checks the armed packets, so a packet keeps its target's object and looks it up in the store
            // again only once the store's generation has moved.
            template <handle_types::type Type, typename T>
            T* wait_packet_target(handle_store<Type, T>& store, wait_completion_packet& wait_packet)
            {
                if (!wait_packet.target_object || wait_packet.target_generation != store.generation())
                {
                    wait_packet.target_object = store.get_by_index(wait_packet.target_object_reference.index);
                    wait_packet.target_generation = store.generation();
                }

                // The object came from this store, so it is a T (a dynamic_cast would cost more than the lookup this saves).
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
                return static_cast<T*>(wait_packet.target_object);
            }

            bool is_wait_packet_target_signaled(windows_emulator& win_emu, wait_completion_packet& wait_packet)
            {
                auto& process = win_emu.process;
                if (!wait_packet.target_object_reference)
                {
                    wait_packet.target_object = nullptr;
                    return false;
                }

                switch (wait_packet.target_object_reference.object_type())
                {
                case handle_types::event:
                    if (const auto* target = wait_packet_target(process.events, wait_packet))
                    {
                        return target->signaled;
                    }
                    break;
                case handle_types::semaphore:
                    if (const auto* target = wait_packet_target(process.semaphores, wait_packet))
                    {
                        return target->current_count > 0;
                    }
                    break;
                case handle_types::mutant:
                    if (const auto* target = wait_packet_target(process.mutants, wait_packet))
                    {
                        return target->locked_count == 0;
                    }
                    break;
                case handle_types::timer:
                    if (auto* target = wait_packet_target(process.timers, wait_packet))
                    {
                        return target->is_signaled(win_emu.clock());
                    }
                    break;
                case handle_types::thread:
                    if (const auto* target = wait_packet_target(process.threads, wait_packet))
                    {
                        return target->is_terminated();
                    }
                    break;
                default:
                    break;
                }

                return is_wait_completion_target_signaled(win_emu, wait_packet.target_object_reference);
            }

            void release_wait_packet_association(process_context& process, wait_completion_packet& wait_packet)
            {
                release_reference(process, wait_packet.io_completion);
                release_reference(process, wait_packet.target_object_reference);
                wait_packet.target_object = nullptr;
                wait_packet.associated = false;
            }

            void enqueue_wait_packet_completion(windows_emulator& win_emu, const object_reference& wait_packet_reference,
                                                const wait_completion_packet& wait_packet)
            {
                auto& process = win_emu.process;
                auto* completion = process.get_object_as<sogen::io_completion>(wait_packet.io_completion);
                if (!completion)
                {
                    return;
                }

                io_completion_message message{};
                message.key_context = wait_packet.key_context;
                message.apc_context = wait_packet.apc_context;
                message.io_status_block = wait_packet.io_status_block;
                message.io_status_block.Information = wait_packet.io_status_information;

                if (const auto retained_wait_packet = process.retain_object(wait_packet_reference))
                {
                    message.wait_packet_reference = *retained_wait_packet;
                }

                insert(win_emu, wait_packet.io_completion, *completion, message);
            }

            // Queues the completion of each signaled wait packet on the port, or on any port without one.
            void queue_signaled_wait_packets(windows_emulator& win_emu, const std::optional<object_reference>& io_completion)
            {
                auto& process = win_emu.process;
                std::vector<std::pair<object_reference, wait_completion_packet*>> signaled{};

                for (auto& [packet_id, wait_packet] : process.wait_completion_packets)
                {
                    if (!wait_packet.associated || wait_packet.queued_completion)
                    {
                        continue;
                    }

                    if (io_completion && wait_packet.io_completion != *io_completion)
                    {
                        continue;
                    }

                    if (!is_wait_packet_target_signaled(win_emu, wait_packet))
                    {
                        continue;
                    }

                    consume_wait_completion_target(process, wait_packet.target_object_reference);
                    wait_packet.queued_completion = true;
                    signaled.emplace_back(object_reference{.type = handle_types::wait_completion_packet, .index = packet_id}, &wait_packet);
                }

                // Queued once the walk is done: a completion handed to a waiter can release its packet.
                for (const auto& [wait_packet_reference, wait_packet] : signaled)
                {
                    enqueue_wait_packet_completion(win_emu, wait_packet_reference, *wait_packet);
                }
            }

            // KiWakeQueueWaiter: the packet is the result of the waiter's wait, written where its syscall asked for it. The
            // buffers were probed before the wait but can have been freed or reprotected since, and the kernel copies the
            // packet out after taking it off the queue (write_message, write_entries).
            void deliver(windows_emulator& win_emu, emulator_thread& thread, const io_completion_message& message)
            {
                const auto wait = *thread.await_io_completion;
                thread.mark_as_ready(
                    wait.type == io_completion_wait_type::remove_single
                        ? write_message(win_emu.memory, wait.key_context_ptr, wait.apc_context_ptr, wait.io_status_block_ptr, message)
                        : write_entries(win_emu.memory, wait.completion_entries_ptr, wait.entries_removed_ptr, {&message, 1}));
                worker_factory_support::on_io_completion_message_dequeued(win_emu, message);

                // NtWaitForWorkViaWorkerFactory checks for more work after its wait, and a packet handed straight to its worker
                // does not go back through that syscall.
                worker_factory_support::check_for_more_work(win_emu, wait.worker_factory_handle);
                clear_wait_packet_completion_state(win_emu.process, message.wait_packet_reference);
                win_emu.wake_idle_vcpu();
            }
        }

        void add_waiter(io_completion& completion, const uint32_t thread_id)
        {
            std::erase(completion.waiters, thread_id);
            completion.waiters.push_back(thread_id);
        }

        void insert(windows_emulator& win_emu, const object_reference& io_completion_reference, io_completion& completion,
                    const io_completion_message& message)
        {
            // With packets queued, a later one waits behind them: a waiter that resumed from a suspension is not back in the
            // wait list until its KeRemoveQueueEx runs again, and that takes the queue's first packet.
            for (auto i = completion.waiters.size(); completion.queue.empty() && i-- > 0;)
            {
                auto* thread = win_emu.process.find_thread_by_id(completion.waiters[i]);
                const auto waits_here = thread && !thread->is_terminated() && thread->await_io_completion &&
                                        thread->await_io_completion->io_completion == io_completion_reference;

                // A suspended waiter is out of the wait list until it resumes; one whose wait ended is gone.
                if (waits_here && thread->suspended != 0)
                {
                    continue;
                }

                completion.waiters.erase(completion.waiters.begin() + static_cast<ptrdiff_t>(i));
                if (waits_here)
                {
                    deliver(win_emu, *thread, message);
                    return;
                }
            }

            completion.enqueue(message);
            worker_factory_support::on_io_completion_queued(win_emu, completion);
        }

        void insert(windows_emulator& win_emu, const handle io_completion_handle, io_completion& completion,
                    const io_completion_message& message)
        {
            const auto reference = win_emu.process.io_completions.reference_of(io_completion_handle);
            if (reference)
            {
                insert(win_emu, *reference, completion, message);
            }
        }

        NTSTATUS probe_completion_entries(memory_manager& memory, const uint64_t entries, const ULONG count, const uint64_t entries_removed)
        {
            constexpr auto entry_size = sizeof(FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>);
            if (entries % 8 != 0)
            {
                return STATUS_DATATYPE_MISALIGNMENT;
            }

            if (!is_writable_range(memory, entries, entry_size * count) || !is_writable_range(memory, entries_removed, sizeof(ULONG)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS write_message(memory_manager& memory, const uint64_t key_context, const uint64_t apc_context,
                               const uint64_t io_status_block, const io_completion_message& message)
        {
            const auto store = [&]<typename T>(const uint64_t address, const T& value) {
                if (!is_writable_range(memory, address, sizeof(T)))
                {
                    return false;
                }

                emulator_object<T>{memory, address}.write(value);
                return true;
            };

            return store(key_context, message.key_context) && store(apc_context, message.apc_context) &&
                           store(io_status_block, message.io_status_block)
                       ? STATUS_SUCCESS
                       : STATUS_ACCESS_VIOLATION;
        }

        NTSTATUS write_entries(memory_manager& memory, const uint64_t entries, const uint64_t entries_removed,
                               const std::span<const io_completion_message> messages)
        {
            using entry_type = FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>;

            auto status = STATUS_SUCCESS;
            for (size_t i = 0; i < messages.size(); ++i)
            {
                const auto entry = entries + i * sizeof(entry_type);
                if (write_message(memory, entry + offsetof(entry_type, KeyContext), entry + offsetof(entry_type, ApcContext),
                                  entry + offsetof(entry_type, IoStatusBlock), messages[i]) != STATUS_SUCCESS)
                {
                    status = STATUS_ACCESS_VIOLATION;
                }
            }

            if (status != STATUS_SUCCESS || !is_writable_range(memory, entries_removed, sizeof(ULONG)))
            {
                return STATUS_ACCESS_VIOLATION;
            }

            emulator_object<ULONG>{memory, entries_removed}.write(static_cast<ULONG>(messages.size()));
            return STATUS_SUCCESS;
        }

        bool is_wait_completion_target_type(const handle target_object_handle)
        {
            switch (target_object_handle.value.type)
            {
            case handle_types::process:
            case handle_types::event:
            case handle_types::thread:
            case handle_types::managed_thread:
            case handle_types::semaphore:
            case handle_types::port:
            case handle_types::mutant:
            case handle_types::timer:
                return true;

            // libuv registers a thread-pool wait on the console input handle to read a raw TTY, so
            // without this a guest cannot start a terminal UI. Only console input and NUL are accepted:
            // a wait on console output never completes on Windows, and a real file object carries no
            // signal the emulator tracks.
            case handle_types::file:
                return target_object_handle == STDIN_HANDLE || target_object_handle == NUL_HANDLE;

            default:
                return false;
            }
        }

        bool retain_reference(process_context& process, const emulator_thread* active_thread, const handle source_handle,
                              object_reference& retained_reference)
        {
            if (source_handle.bits == 0)
            {
                retained_reference = {};
                return true;
            }

            const auto resolved_source_handle = process.resolve_object_pseudo_handle(source_handle, active_thread);

            if (resolved_source_handle.value.is_pseudo)
            {
                retained_reference = object_reference{.type = static_cast<handle_types::type>(resolved_source_handle.value.type),
                                                      .pseudo = resolved_source_handle};
                return true;
            }

            auto* store = process.get_handle_store(resolved_source_handle);
            if (!store)
            {
                return false;
            }

            const auto retained = store->retain(resolved_source_handle);
            if (!retained)
            {
                return false;
            }

            retained_reference = *retained;
            return true;
        }

        bool retain_reference(process_context& process, const object_reference& source_reference, object_reference& retained_reference)
        {
            if (!source_reference)
            {
                retained_reference = {};
                return true;
            }

            const auto retained = process.retain_object(source_reference);
            if (!retained)
            {
                return false;
            }

            retained_reference = *retained;
            return true;
        }

        void release_reference(process_context& process, object_reference& retained_reference)
        {
            process.release_object(retained_reference);
        }

        void clear_wait_packet_completion_state(process_context& process, object_reference wait_packet_reference)
        {
            if (!wait_packet_reference)
            {
                return;
            }

            if (auto* wait_packet = process.get_object_as<wait_completion_packet>(wait_packet_reference))
            {
                release_wait_packet_association(process, *wait_packet);
                wait_packet->queued_completion = false;
            }

            // Each queued completion keeps an extra packet reference.
            process.release_object(wait_packet_reference);
        }

        void cleanup_wait_packet_on_close(process_context& process, const handle wait_packet_handle)
        {
            auto* wait_packet = process.wait_completion_packets.get(wait_packet_handle);
            if (!wait_packet)
            {
                return;
            }

            const auto wait_packet_reference = process.wait_completion_packets.reference_of(wait_packet_handle);
            if (!wait_packet_reference)
            {
                return;
            }

            if (wait_packet->queued_completion)
            {
                if (auto* completion = process.get_object_as<sogen::io_completion>(wait_packet->io_completion))
                {
                    if (auto message = completion->remove_by_wait_packet(*wait_packet_reference))
                    {
                        process.release_object(message->wait_packet_reference);
                    }
                }
            }

            release_wait_packet_association(process, *wait_packet);
            wait_packet->queued_completion = false;
        }

        void materialize_signaled_wait_packets(windows_emulator& win_emu, const object_reference& io_completion)
        {
            if (win_emu.process.get_object_as<sogen::io_completion>(io_completion))
            {
                queue_signaled_wait_packets(win_emu, io_completion);
            }
        }

        void materialize_signaled_wait_packets(windows_emulator& win_emu, const handle io_completion_handle)
        {
            const auto reference = win_emu.process.io_completions.reference_of(io_completion_handle);
            if (reference)
            {
                materialize_signaled_wait_packets(win_emu, *reference);
            }
        }

        void materialize_all_signaled_wait_packets(windows_emulator& win_emu)
        {
            queue_signaled_wait_packets(win_emu, std::nullopt);
        }

        bool dequeue_io_completion_message(windows_emulator& win_emu, const object_reference& io_completion,
                                           io_completion_message& out_message)
        {
            auto& process = win_emu.process;
            auto* completion = process.get_object_as<sogen::io_completion>(io_completion);
            if (!completion)
            {
                return false;
            }

            materialize_signaled_wait_packets(win_emu, io_completion);

            if (!completion->dequeue(out_message))
            {
                return false;
            }

            worker_factory_support::on_io_completion_message_dequeued(win_emu, out_message);
            clear_wait_packet_completion_state(process, out_message.wait_packet_reference);
            return true;
        }

        bool dequeue_io_completion_message(windows_emulator& win_emu, const handle io_completion_handle, io_completion_message& out_message)
        {
            const auto reference = win_emu.process.io_completions.reference_of(io_completion_handle);
            return reference && dequeue_io_completion_message(win_emu, *reference, out_message);
        }

        std::vector<io_completion_message> dequeue_io_completion_entries(windows_emulator& win_emu, const object_reference& io_completion,
                                                                         const ULONG max_count)
        {
            auto& process = win_emu.process;
            std::vector<io_completion_message> messages{};
            auto* completion = process.get_object_as<sogen::io_completion>(io_completion);
            if (!completion)
            {
                return messages;
            }

            materialize_signaled_wait_packets(win_emu, io_completion);

            io_completion_message message{};
            while (messages.size() < max_count && completion->dequeue(message))
            {
                worker_factory_support::on_io_completion_message_dequeued(win_emu, message);
                clear_wait_packet_completion_state(process, message.wait_packet_reference);
                messages.push_back(message);
            }

            return messages;
        }

        std::vector<io_completion_message> dequeue_io_completion_entries(windows_emulator& win_emu, const handle io_completion_handle,
                                                                         const ULONG max_count)
        {
            const auto reference = win_emu.process.io_completions.reference_of(io_completion_handle);
            return reference ? dequeue_io_completion_entries(win_emu, *reference, max_count) : std::vector<io_completion_message>{};
        }
    }

} // namespace sogen
