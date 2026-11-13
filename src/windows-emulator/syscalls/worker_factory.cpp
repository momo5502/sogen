#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../io_completion_wait.hpp"
#include "../syscall_utils.hpp"
#include "../worker_factory_support.hpp"

#include <limits>

namespace sogen
{
    namespace worker_factory_support
    {
        bool enqueue_release_completion(windows_emulator& win_emu, const handle worker_factory_handle)
        {
            auto& process = win_emu.process;
            auto* factory = process.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return false;
            }

            auto* completion = process.io_completions.get(factory->io_completion_handle);
            if (!completion)
            {
                return false;
            }

            io_completion_message message{};
            message.io_status_block.Status = STATUS_SUCCESS;
            message.io_status_block.Information = 0;
            message.worker_factory_handle = worker_factory_handle;
            message.worker_factory_release = true;
            io_completion_wait::insert(win_emu, factory->io_completion_handle, *completion, message);
            return true;
        }

        void on_io_completion_message_dequeued(windows_emulator& win_emu, const io_completion_message& message)
        {
            if (!message.worker_factory_release || message.worker_factory_handle.bits == 0)
            {
                return;
            }

            auto* factory = win_emu.process.worker_factories.get(message.worker_factory_handle);
            if (!factory)
            {
                return;
            }

            if (factory->pending_release_count > 0)
            {
                --factory->pending_release_count;
            }

            if (factory->shutdown || factory->pending_release_count == 0)
            {
                factory->release_pending = false;
                return;
            }

            if (!enqueue_release_completion(win_emu, message.worker_factory_handle))
            {
                factory->release_pending = false;
            }
        }
    }

    namespace syscalls
    {
        namespace
        {
            std::u16string read_object_name(const syscall_context& c,
                                            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
            {
                if (!object_attributes)
                {
                    return {};
                }

                const auto attributes = object_attributes.read();
                if (attributes.ObjectName == 0)
                {
                    return {};
                }

                return read_unicode_string(c.emu, attributes.ObjectName);
            }

            void prune_worker_factory_threads(windows_emulator& win_emu, worker_factory& factory)
            {
                std::erase_if(factory.worker_threads, [&](const handle thread_handle) {
                    const auto* thread = win_emu.process.threads.get(thread_handle);
                    return thread == nullptr || thread->is_terminated();
                });
            }

            uint32_t get_worker_factory_thread_limit(const worker_factory& factory)
            {
                uint32_t limit = std::numeric_limits<uint32_t>::max();

                if (factory.max_thread_count != 0)
                {
                    limit = std::min(limit, factory.max_thread_count);
                }

                if (factory.thread_maximum != 0)
                {
                    limit = std::min(limit, factory.thread_maximum);
                }

                if (factory.thread_soft_maximum != 0)
                {
                    limit = std::min(limit, factory.thread_soft_maximum);
                }

                return limit;
            }

            void create_worker_factory_thread(windows_emulator& win_emu, worker_factory& factory)
            {
                const auto stack_size =
                    factory.stack_reserve != 0 ? factory.stack_reserve : win_emu.mod_manager.executable->size_of_stack_reserve;
                const auto create_flags = (factory.flags & WORKER_FACTORY_FLAG_LOADER_POOL) ? THREAD_CREATE_FLAGS_LOADER_WORKER : 0;
                const auto stack_commit_size =
                    factory.stack_commit != 0 ? factory.stack_commit : win_emu.mod_manager.executable->size_of_stack_commit;
                const auto thread_handle = win_emu.process.create_thread(win_emu.memory, factory.start_routine, factory.start_parameter,
                                                                         stack_size, create_flags, false, stack_commit_size);
                factory.worker_threads.push_back(thread_handle);

                // Like ExpWorkerFactoryCreateThread: the new worker stays pending until it calls NtWorkerFactoryWorkerReady.
                ++factory.pending_worker_count;
                win_emu.wake_idle_vcpu();
            }

            void ensure_worker_factory_threads(const syscall_context& c, worker_factory& factory)
            {
                if (factory.shutdown || factory.paused != 0 || factory.start_routine == 0)
                {
                    return;
                }

                prune_worker_factory_threads(c.win_emu, factory);

                const auto limit = get_worker_factory_thread_limit(factory);
                if (limit == 0)
                {
                    return;
                }

                // Bindings start no worker by themselves: see try_enter_away_mode.
                const auto desired = std::min(factory.thread_minimum, limit);
                while (factory.worker_threads.size() < desired)
                {
                    create_worker_factory_thread(c.win_emu, factory);
                }
            }

            bool has_waiting_worker(windows_emulator& win_emu, const worker_factory& factory)
            {
                return std::ranges::any_of(factory.worker_threads, [&](const handle thread_handle) {
                    const auto* thread = win_emu.process.threads.get(thread_handle);
                    return thread && thread->await_io_completion &&
                           thread->await_io_completion->io_completion_handle == factory.io_completion_handle;
                });
            }

            // ExpLeaveWorkerFactoryAwayMode
            void leave_away_mode(worker_factory& factory)
            {
                factory.away_notification = false;
                factory.away_mode = false;
            }

            // ExpTryEnterWorkerFactoryAwayMode: says whether to look for a worker now. A factory with bindings and no worker
            // waiting on its port is away; with nothing queued there it starts no worker yet and waits for the port's next
            // packet instead (on_io_completion_queued).
            bool try_enter_away_mode(windows_emulator& win_emu, worker_factory& factory)
            {
                prune_worker_factory_threads(win_emu, factory);
                if (factory.worker_threads.size() >= factory.thread_maximum || has_waiting_worker(win_emu, factory))
                {
                    return false;
                }

                if (factory.binding_count == 0)
                {
                    return true;
                }

                factory.away_mode = true;
                const auto* completion = win_emu.process.io_completions.get(factory.io_completion_handle);
                if (completion && !completion->queue.empty())
                {
                    return true;
                }

                factory.away_notification = true;
                return false;
            }

            // Windows ExpWorkerFactoryCheckCreate: start one more worker when work is queued (a completion or a
            // release not yet taken), no worker waits on the port for it, no new worker is still starting and the
            // factory is under its maximum. Without it a pool whose workers are all inside long callbacks never runs
            // newly posted work.
            // ponytail: creates at once where Windows defers 30-150 ms (a processor's worth of workers running, recent
            // thread churn, above the soft maximum); add a deferred-create timer if a guest ever depends on that pacing.
            void check_create_worker_factory_thread(windows_emulator& win_emu, worker_factory& factory)
            {
                if (factory.shutdown || factory.paused != 0 || factory.start_routine == 0 || factory.pending_worker_count != 0)
                {
                    return;
                }

                prune_worker_factory_threads(win_emu, factory);
                if (factory.worker_threads.size() >= factory.thread_maximum)
                {
                    return;
                }

                const auto* completion = win_emu.process.io_completions.get(factory.io_completion_handle);
                const auto work_queued = factory.pending_release_count != 0 || (completion && !completion->queue.empty());
                if (work_queued && !has_waiting_worker(win_emu, factory))
                {
                    leave_away_mode(factory);
                    create_worker_factory_thread(win_emu, factory);
                }
            }

            // The end of NtWaitForWorkViaWorkerFactory once its worker took work.
            void check_for_more_work(windows_emulator& win_emu, worker_factory& factory)
            {
                if (try_enter_away_mode(win_emu, factory))
                {
                    check_create_worker_factory_thread(win_emu, factory);
                }
            }
        }

        NTSTATUS handle_NtCreateWorkerFactory(const syscall_context& c, const emulator_object<handle> worker_factory_handle,
                                              const ACCESS_MASK /*desired_access*/,
                                              const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              const handle io_completion_handle, const handle worker_process_handle,
                                              const emulator_pointer start_routine, const emulator_pointer start_parameter,
                                              const ULONG max_thread_count,
                                              const EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) stack_reserve,
                                              const EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) stack_commit)
        {
            if (!c.proc.io_completions.get(io_completion_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            if (!c.proc.is_current_process_handle(worker_process_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto name = read_object_name(c, object_attributes);
            if (!name.empty())
            {
                for (auto& entry : c.proc.worker_factories)
                {
                    if (entry.second.name == name)
                    {
                        ++entry.second.ref_count;
                        worker_factory_handle.write(c.proc.worker_factories.make_handle(entry.first));
                        return STATUS_OBJECT_NAME_EXISTS;
                    }
                }
            }

            worker_factory factory{};
            factory.name = name;
            factory.inherit = object_attributes && (object_attributes.read().Attributes & OBJ_INHERIT) != 0;

            handle retained_io_completion_handle{};
            if (!io_completion_wait::retain_handle_reference(c.proc, c.vcpu.active_thread, io_completion_handle,
                                                             retained_io_completion_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            factory.io_completion_handle = retained_io_completion_handle;
            factory.worker_process_handle = worker_process_handle;
            factory.start_routine = start_routine;
            factory.start_parameter = start_parameter;
            factory.max_thread_count = max_thread_count;
            factory.stack_reserve = stack_reserve;
            factory.stack_commit = stack_commit;
            factory.thread_maximum = max_thread_count;

            auto [stored_handle, stored_factory] = c.proc.worker_factories.store_and_get(std::move(factory));
            ensure_worker_factory_threads(c, *stored_factory);
            worker_factory_handle.write(stored_handle);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtWorkerFactoryWorkerReady(const syscall_context& c, const handle worker_factory_handle)
        {
            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            // A worker counts as started once it reports in; with none starting, Windows fails the call.
            if (factory->pending_worker_count == 0)
            {
                return STATUS_UNSUCCESSFUL;
            }

            --factory->pending_worker_count;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                      const WORKERFACTORYINFOCLASS info_class,
                                                      const emulator_pointer worker_factory_information,
                                                      const ULONG worker_factory_information_length)
        {
            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (info_class >= MaxWorkerFactoryInfoClass)
            {
                return STATUS_INVALID_INFO_CLASS;
            }

            if (worker_factory_information == 0 && worker_factory_information_length != 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            factory->last_info_class = static_cast<ULONG>(info_class);
            factory->last_info_length = worker_factory_information_length;
            factory->last_info_value = 0;

            const emulator_object<LARGE_INTEGER> value_i64{c.emu, worker_factory_information};
            const emulator_object<ULONG> value_u32{c.emu, worker_factory_information};
            const emulator_object<uint64_t> value_u64{c.emu, worker_factory_information};

            switch (info_class)
            {
            case WorkerFactoryStackInformation: {
                struct stack_information
                {
                    uint64_t reserve;
                    uint64_t commit;
                };

                if (worker_factory_information_length != sizeof(stack_information))
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                stack_information information{};
                if (!c.emu.try_read_memory(worker_factory_information, &information, sizeof(information)))
                {
                    return STATUS_ACCESS_VIOLATION;
                }
                factory->stack_reserve = information.reserve;
                factory->stack_commit = information.commit;
                return STATUS_SUCCESS;
            }

            case WorkerFactoryTimeout:
            case WorkerFactoryRetryTimeout:
            case WorkerFactoryIdleTimeout: {
                if (worker_factory_information_length != sizeof(LARGE_INTEGER))
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const auto value = value_i64.read().QuadPart;
                factory->last_info_value = static_cast<uint64_t>(value);

                if (info_class == WorkerFactoryTimeout)
                {
                    factory->timeout = value;
                }
                else if (info_class == WorkerFactoryRetryTimeout)
                {
                    factory->retry_timeout = value;
                }
                else
                {
                    factory->idle_timeout = value;
                }
                return STATUS_SUCCESS;
            }

            case WorkerFactoryBindingCount: {
                if (worker_factory_information_length != sizeof(ULONG))
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const auto raw_value = value_u32.read();
                const auto delta = static_cast<LONG>(raw_value);
                factory->last_info_value = raw_value;
                const auto previous = factory->binding_count;

                if (delta < 0)
                {
                    const auto decrement = static_cast<uint32_t>(-delta);
                    if (decrement >= factory->binding_count)
                    {
                        factory->binding_count = 0;
                    }
                    else
                    {
                        factory->binding_count -= decrement;
                    }
                }
                else
                {
                    const auto increment = static_cast<uint32_t>(delta);
                    const auto next = static_cast<uint64_t>(factory->binding_count) + increment;
                    factory->binding_count =
                        next > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(next);
                }

                // The first binding makes the factory wait for work instead of starting a worker, the last one ends that.
                if (previous != 0 && factory->binding_count == 0)
                {
                    leave_away_mode(*factory);
                }
                else if (previous == 0 && factory->binding_count != 0 && try_enter_away_mode(c.win_emu, *factory))
                {
                    check_create_worker_factory_thread(c.win_emu, *factory);
                }

                return STATUS_SUCCESS;
            }

            case WorkerFactoryThreadMinimum:
            case WorkerFactoryThreadMaximum:
            case WorkerFactoryPaused:
            case WorkerFactoryThreadBasePriority:
            case WorkerFactoryTimeoutWaiters:
            case WorkerFactoryFlags:
            case WorkerFactoryThreadSoftMaximum: {
                if (worker_factory_information_length != sizeof(ULONG))
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const auto value = value_u32.read();
                factory->last_info_value = value;

                switch (info_class)
                {
                case WorkerFactoryThreadMinimum:
                    // A raised minimum starts its workers itself.
                    if (value > factory->thread_minimum)
                    {
                        leave_away_mode(*factory);
                    }
                    factory->thread_minimum = value;
                    break;
                case WorkerFactoryThreadMaximum: {
                    // Work waiting for a worker: a queued packet, or a release while there was no maximum.
                    const auto* completion = c.proc.io_completions.get(factory->io_completion_handle);
                    const auto work_queued =
                        (factory->thread_maximum == 0 && factory->pending_release_count != 0) || (completion && !completion->queue.empty());
                    factory->thread_maximum = value;

                    if (value == 0 || !work_queued || factory->shutdown || factory->paused != 0 || factory->start_routine == 0)
                    {
                        break;
                    }

                    // A maximum with room for it starts one worker for that work, unless one is still starting.
                    prune_worker_factory_threads(c.win_emu, *factory);
                    if (factory->pending_worker_count == 0 && factory->worker_threads.size() < factory->thread_maximum)
                    {
                        leave_away_mode(*factory);
                        create_worker_factory_thread(c.win_emu, *factory);
                    }
                    break;
                }
                case WorkerFactoryPaused:
                    factory->paused = value;
                    break;
                case WorkerFactoryThreadBasePriority:
                    factory->thread_base_priority = value;
                    break;
                case WorkerFactoryTimeoutWaiters:
                    factory->timeout_waiters = value;
                    break;
                case WorkerFactoryFlags:
                    factory->flags = value;
                    break;
                case WorkerFactoryThreadSoftMaximum:
                    factory->thread_soft_maximum = value;
                    break;
                default:
                    break;
                }

                ensure_worker_factory_threads(c, *factory);
                return STATUS_SUCCESS;
            }

            default: {
                if (worker_factory_information_length == 0)
                {
                    return STATUS_SUCCESS;
                }

                if (worker_factory_information_length == sizeof(ULONG))
                {
                    factory->last_info_value = value_u32.read();
                    return STATUS_SUCCESS;
                }

                if (worker_factory_information_length == sizeof(uint64_t))
                {
                    factory->last_info_value = value_u64.read();
                    return STATUS_SUCCESS;
                }

                return STATUS_NOT_SUPPORTED;
            }
            }
        }

        NTSTATUS handle_NtShutdownWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                const emulator_object<LONG> pending_worker_count)
        {
            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            factory->shutdown = true;
            leave_away_mode(*factory);
            factory->worker_threads.clear();
            pending_worker_count.write_if_valid(0);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtReleaseWorkerFactoryWorker(const syscall_context& c, const handle worker_factory_handle)
        {
            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (factory->pending_release_count == std::numeric_limits<uint32_t>::max())
            {
                return STATUS_UNSUCCESSFUL;
            }

            ++factory->pending_release_count;
            if (!factory->release_pending)
            {
                factory->release_pending = true;
                // The worker for the release packet is looked for right after it is queued.
                leave_away_mode(*factory);

                if (!worker_factory_support::enqueue_release_completion(c.win_emu, worker_factory_handle))
                {
                    factory->release_pending = false;
                    --factory->pending_release_count;
                    return STATUS_INVALID_HANDLE;
                }

                c.win_emu.wake_idle_vcpu();

                // Like the kernel, look for a free worker only when the release packet is newly queued.
                check_create_worker_factory_thread(c.win_emu, *factory);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtWaitForWorkViaWorkerFactory(
            const syscall_context& c, const handle worker_factory_handle,
            const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> mini_packets, const ULONG count,
            const emulator_object<ULONG> packets_returned, const emulator_pointer /*deferred_work*/)
        {
            // As NtRemoveIoCompletionEx: up to 0x7FFFFFF mini packets, whose array and returned count are probed before the
            // handle is looked at (19041).
            constexpr ULONG max_count = 0x7FFFFFF;
            if (count == 0 || count > max_count || !mini_packets)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (const auto status =
                    io_completion_wait::probe_completion_entries(c.win_emu.memory, mini_packets.value(), count, packets_returned.value());
                status != STATUS_SUCCESS)
            {
                return status;
            }

            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            auto* completion = c.proc.io_completions.get(factory->io_completion_handle);
            if (!completion)
            {
                return STATUS_INVALID_HANDLE;
            }

            // A worker back for work takes the port's next packet itself.
            leave_away_mode(*factory);

            const auto messages = io_completion_wait::dequeue_io_completion_entries(c.win_emu, factory->io_completion_handle, count);
            if (!messages.empty())
            {
                // A worker taking work makes sure another one is coming for whatever is still queued, or for what comes next.
                check_for_more_work(c.win_emu, *factory);
                return io_completion_wait::write_entries(c.win_emu.memory, mini_packets.value(), packets_returned.value(), messages);
            }

            packets_returned.write_if_valid(0);

            auto& t = c.thread();
            t.await_objects = {};
            t.await_any = false;
            t.await_time = {};
            t.await_msg = {};
            t.waiting_for_alert = false;
            t.await_io_completion = pending_io_completion_wait{};

            auto& wait = *t.await_io_completion;
            wait.type = io_completion_wait_type::remove_multiple;
            wait.io_completion_handle = factory->io_completion_handle;
            wait.worker_factory_handle = worker_factory_handle;
            wait.completion_entries_ptr = mini_packets.value();
            wait.entries_removed_ptr = packets_returned.value();
            wait.max_entries = count;

            if (factory->timeout != 0)
            {
                LARGE_INTEGER timeout{};
                timeout.QuadPart = factory->timeout;
                wait.timeout = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout);
                wait.timeout_absolute = utils::is_absolute_due_time(timeout);

                constexpr auto infinite = std::chrono::steady_clock::time_point::min();
                if (wait.timeout.has_value() && wait.timeout.value() != infinite && wait.timeout.value() < c.win_emu.clock().steady_now())
                {
                    packets_returned.write_if_valid(0);
                    t.await_io_completion = {};
                    return STATUS_TIMEOUT;
                }
            }

            io_completion_wait::add_waiter(*completion, t.id);
            c.win_emu.yield_thread(c.vcpu, false);
            return STATUS_SUCCESS;
        }
    }

    namespace worker_factory_support
    {
        void check_for_more_work(windows_emulator& win_emu, const handle worker_factory_handle)
        {
            if (auto* factory = win_emu.process.worker_factories.get(worker_factory_handle))
            {
                syscalls::check_for_more_work(win_emu, *factory);
            }
        }

        void on_io_completion_queued(windows_emulator& win_emu, const io_completion& completion)
        {
            // ponytail: walks the process's few worker factories for each queued packet; keep the waiting factories on the
            // port if a guest ever runs many pools.
            for (auto& [index, factory] : win_emu.process.worker_factories)
            {
                if (!factory.away_notification || win_emu.process.io_completions.get(factory.io_completion_handle) != &completion)
                {
                    continue;
                }

                // The notification fires once: the packet starts a worker if the factory still has none free for it.
                factory.away_notification = false;
                if (factory.away_mode && syscalls::try_enter_away_mode(win_emu, factory))
                {
                    syscalls::check_create_worker_factory_thread(win_emu, factory);
                }
            }
        }
    }

} // namespace sogen
