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
        bool enqueue_release_completion(windows_emulator& win_emu, const object_reference& worker_factory)
        {
            auto& process = win_emu.process;
            auto* factory = process.get_object_as<sogen::worker_factory>(worker_factory);
            if (!factory)
            {
                return false;
            }

            auto* completion = process.get_object_as<io_completion>(factory->io_completion);
            if (!completion)
            {
                return false;
            }

            io_completion_message message{};
            message.io_status_block.Status = STATUS_SUCCESS;
            message.io_status_block.Information = 0;
            message.worker_factory = worker_factory;
            message.worker_factory_release = true;
            io_completion_wait::insert(win_emu, factory->io_completion, *completion, message);
            return true;
        }

        void on_io_completion_message_dequeued(windows_emulator& win_emu, const io_completion_message& message,
                                               const object_reference& waiting_worker_factory)
        {
            if (message.worker_factory_shutdown)
            {
                continue_shutdown_wake(win_emu, message.worker_factory, waiting_worker_factory == message.worker_factory);
                return;
            }

            if (!message.worker_factory_release || !message.worker_factory)
            {
                return;
            }

            auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(message.worker_factory);
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

            if (!enqueue_release_completion(win_emu, message.worker_factory))
            {
                factory->release_pending = false;
            }
        }
    }

    namespace syscalls
    {
        namespace
        {
            constexpr int64_t default_worker_idle_timeout = -670'000'000;
            constexpr int64_t minimum_worker_idle_timeout = -10'000'000;

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

            // The factory's counts are the ready workers (the worker list without the ones still starting) less the workers
            // that called class 9 type 3, and of those the active ones, which type 2 takes workers out of and type 0 puts back.
            // The pending count is the factory's own: a starting worker that exits keeps it raised until the factory is shut down.
            size_t count_total_workers(const worker_factory& factory)
            {
                const auto size = factory.worker_threads.size();
                const auto ready = size - std::min<size_t>(size, factory.starting_workers.size());
                return ready - std::min<size_t>(ready, factory.removed_worker_count);
            }

            size_t count_active_workers(const worker_factory& factory)
            {
                const auto total = count_total_workers(factory);
                return total - std::min<size_t>(total, factory.inactive_worker_count);
            }

            size_t count_active_and_starting_workers(const worker_factory& factory)
            {
                return count_active_workers(factory) + factory.pending_worker_count;
            }

            // A worker that set type 3 is out of the count for good: only its own departure gives the removal back.
            void forget_departed_workers(worker_factory& factory, const std::vector<handle>& departed)
            {
                for (const auto thread_handle : departed)
                {
                    std::erase(factory.starting_workers, thread_handle);
                    if (std::erase(factory.removed_workers, thread_handle) != 0 && factory.removed_worker_count != 0)
                    {
                        --factory.removed_worker_count;
                    }
                }

                factory.inactive_worker_count =
                    static_cast<uint32_t>(std::min<size_t>(factory.inactive_worker_count, count_total_workers(factory)));
            }

            void prune_worker_factory_threads(windows_emulator& win_emu, worker_factory& factory)
            {
                std::vector<handle> departed{};
                std::erase_if(factory.worker_threads, [&](const handle thread_handle) {
                    const auto* thread = win_emu.process.threads.get(thread_handle);
                    const auto gone = thread == nullptr || thread->is_terminated();
                    if (gone)
                    {
                        departed.push_back(thread_handle);
                    }

                    return gone;
                });
                forget_departed_workers(factory, departed);
            }

            bool create_worker_factory_thread(windows_emulator& win_emu, worker_factory& factory)
            {
                const auto stack_size =
                    factory.stack_reserve != 0 ? factory.stack_reserve : win_emu.mod_manager.executable->size_of_stack_reserve;
                const auto create_flags = (factory.flags & WORKER_FACTORY_FLAG_LOADER_POOL) ? THREAD_CREATE_FLAGS_LOADER_WORKER : 0;
                const auto stack_commit_size =
                    factory.stack_commit != 0 ? factory.stack_commit : win_emu.mod_manager.executable->size_of_stack_commit;
                const auto thread_handle = win_emu.process.create_thread(win_emu.memory, factory.start_routine, factory.start_parameter,
                                                                         stack_size, create_flags, false, stack_commit_size);
                if (thread_handle == 0)
                {
                    return false;
                }
                factory.worker_threads.push_back(thread_handle);
                factory.starting_workers.push_back(thread_handle);
                factory.worker_created = true;

                // Like ExpWorkerFactoryCreateThread: the new worker stays pending until it calls NtWorkerFactoryWorkerReady.
                ++factory.pending_worker_count;
                win_emu.wake_idle_vcpu();
                return true;
            }

            NTSTATUS ensure_worker_factory_threads(const syscall_context& c, worker_factory& factory)
            {
                if (factory.shutdown || factory.start_routine == 0)
                {
                    return STATUS_SUCCESS;
                }

                prune_worker_factory_threads(c.win_emu, factory);

                // Only a raised thread minimum starts workers at once, and the maximum rises with it; bindings start
                // none by themselves (try_enter_away_mode).
                while (count_active_and_starting_workers(factory) < factory.thread_minimum)
                {
                    if (!create_worker_factory_thread(c.win_emu, factory))
                    {
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                }
                return STATUS_SUCCESS;
            }

            // The kernel's waiting count is raised by every NtWaitForWorkViaWorkerFactory wait on the factory, whoever the caller is, and
            // not by a plain removal from the same port.
            size_t count_factory_waits(windows_emulator& win_emu, const object_reference& worker_factory)
            {
                return static_cast<size_t>(std::ranges::count_if(win_emu.process.threads, [&](const auto& entry) {
                    const auto& thread = entry.second;
                    return !thread.is_terminated() && thread.await_io_completion &&
                           thread.await_io_completion->worker_factory == worker_factory;
                }));
            }

            size_t count_waiting_workers(windows_emulator& win_emu, const worker_factory& factory)
            {
                return static_cast<size_t>(std::ranges::count_if(factory.worker_threads, [&](const handle thread_handle) {
                    const auto* thread = win_emu.process.threads.get(thread_handle);
                    return thread && thread->await_io_completion && thread->await_io_completion->io_completion == factory.io_completion;
                }));
            }

            bool has_waiting_worker(windows_emulator& win_emu, const worker_factory& factory)
            {
                return count_waiting_workers(win_emu, factory) != 0;
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
                if (count_active_and_starting_workers(factory) >= factory.thread_maximum || has_waiting_worker(win_emu, factory))
                {
                    return false;
                }

                if (factory.binding_count == 0)
                {
                    return true;
                }

                factory.away_mode = true;
                const auto* completion = win_emu.process.get_object_as<io_completion>(factory.io_completion);
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
                if (factory.shutdown || factory.start_routine == 0 || factory.pending_worker_count != 0)
                {
                    return;
                }

                prune_worker_factory_threads(win_emu, factory);
                if (count_active_and_starting_workers(factory) >= factory.thread_maximum)
                {
                    return;
                }

                const auto* completion = win_emu.process.get_object_as<io_completion>(factory.io_completion);
                const auto work_queued = factory.pending_release_count != 0 || (completion && !completion->queue.empty());
                if (work_queued && !has_waiting_worker(win_emu, factory))
                {
                    leave_away_mode(factory);
                    (void)create_worker_factory_thread(win_emu, factory);
                }
            }

            void arm_idle_deadline(windows_emulator& win_emu, const worker_factory& factory, pending_io_completion_wait& wait)
            {
                wait.timeout.reset();
                if (factory.idle_timeout == 0)
                {
                    return;
                }

                LARGE_INTEGER idle{};
                idle.QuadPart = factory.idle_timeout;
                wait.timeout = utils::convert_delay_interval_to_time_point(win_emu.clock(), idle);
                wait.timeout_absolute = utils::is_absolute_due_time(idle);
            }

            // Class 9 moves the count of active workers for a worker's callback: type 0 counts a worker active again, 1 asks
            // for one more worker, 2 takes a worker out of the active count and 3 also out of the factory's total.
            NTSTATUS adjust_active_workers(const syscall_context& c, worker_factory& factory, const ULONG callback_type)
            {
                constexpr ULONG count_active_again = 0;
                constexpr ULONG add_worker = 1;
                constexpr ULONG leave_active = 2;
                constexpr ULONG leave_factory = 3;

                prune_worker_factory_threads(c.win_emu, factory);
                auto active = count_active_workers(factory);

                switch (callback_type)
                {
                case count_active_again:
                    if (factory.inactive_worker_count == 0)
                    {
                        return STATUS_UNSUCCESSFUL;
                    }

                    --factory.inactive_worker_count;
                    return STATUS_SUCCESS;

                case add_worker:
                    if (factory.shutdown)
                    {
                        return STATUS_ABANDONED;
                    }

                    if (active >= factory.thread_maximum)
                    {
                        return STATUS_TOO_MANY_THREADS;
                    }

                    break;

                case leave_active:
                case leave_factory:
                    // A factory without an active worker answers this on 26300.
                    if (active == 0)
                    {
                        return STATUS_UNSUCCESSFUL;
                    }

                    if (callback_type == leave_active)
                    {
                        ++factory.inactive_worker_count;
                    }
                    else
                    {
                        ++factory.removed_worker_count;
                        const auto caller = c.proc.threads.find_handle(c.vcpu.active_thread);
                        const auto has = [&](const std::vector<handle>& threads) {
                            return std::ranges::find(threads, caller) != threads.end();
                        };
                        if (has(factory.worker_threads) && !has(factory.removed_workers))
                        {
                            factory.removed_workers.push_back(caller);
                        }
                    }

                    --active;
                    if (active != 0)
                    {
                        return STATUS_SUCCESS;
                    }
                    break;

                default:
                    return STATUS_INVALID_PARAMETER;
                }

                if (factory.shutdown)
                {
                    return STATUS_ABANDONED;
                }

                if (factory.pending_worker_count != 0 || active + factory.pending_worker_count >= factory.thread_maximum)
                {
                    return STATUS_SUCCESS;
                }

                leave_away_mode(factory);
                return create_worker_factory_thread(c.win_emu, factory) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
            }

            // NtSetInformationWorkerFactory checks the class, the length and the buffer before it looks at the handle.
            NTSTATUS validate_set_information(windows_emulator& win_emu, const WORKERFACTORYINFOCLASS info_class,
                                              const emulator_pointer information, const ULONG length)
            {
                // The class 15 length is capped at one page of ids.
                constexpr ULONG cpu_sets_limit = 0x100;

                size_t required_length{};
                switch (static_cast<ULONG>(info_class))
                {
                case WorkerFactoryTimeout:
                case WorkerFactoryRetryTimeout:
                case WorkerFactoryBasicInformation:
                    return STATUS_INVALID_INFO_CLASS;

                case WorkerFactoryPaused:
                    return STATUS_NOT_IMPLEMENTED;

                case WorkerFactoryIdleTimeout:
                    required_length = sizeof(LARGE_INTEGER);
                    break;

                case WorkerFactoryStackInformation:
                    required_length = 2 * sizeof(uint64_t);
                    break;

                case WorkerFactoryBindingCount:
                case WorkerFactoryThreadMinimum:
                case WorkerFactoryThreadMaximum:
                case WorkerFactoryAdjustThreadGoal:
                case WorkerFactoryCallbackType:
                case WorkerFactoryThreadBasePriority:
                case WorkerFactoryTimeoutWaiters:
                case WorkerFactoryFlags:
                case WorkerFactoryThreadSoftMaximum:
                    required_length = sizeof(ULONG);
                    break;

                case WorkerFactoryThreadCpuSets:
                    if (length % sizeof(uint64_t) != 0 || (length >= cpu_sets_limit && length != cpu_sets_limit))
                    {
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }

                    required_length = length;
                    break;

                default:
                    return STATUS_INVALID_INFO_CLASS;
                }

                if (length != required_length)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                return required_length == 0 ? STATUS_SUCCESS : probe_range(win_emu, information, required_length, memory_permission::read);
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
            if (const auto status = validate_object_attributes(object_attributes); status != STATUS_SUCCESS)
            {
                return status;
            }

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
                        worker_factory_handle.write(
                            c.proc.worker_factories.open_by_index(entry.first, get_handle_attributes(object_attributes)));
                        return STATUS_OBJECT_NAME_EXISTS;
                    }
                }
            }

            worker_factory factory{};
            factory.idle_timeout = default_worker_idle_timeout;
            factory.name = name;
            factory.inherit = object_attributes && (object_attributes.read().Attributes & OBJ_INHERIT) != 0;

            object_reference retained_io_completion{};
            if (!io_completion_wait::retain_reference(c.proc, c.vcpu.active_thread, io_completion_handle, retained_io_completion))
            {
                return STATUS_INVALID_HANDLE;
            }

            factory.io_completion = retained_io_completion;
            factory.worker_process_handle = worker_process_handle;
            factory.start_routine = start_routine;
            factory.start_parameter = start_parameter;
            factory.stack_reserve = stack_reserve;
            factory.stack_commit = stack_commit;
            // ntoskrnl keeps MaxThreadCount as the thread maximum, with a minimum of 0: the factory starts no worker yet.
            factory.thread_maximum = max_thread_count;

            worker_factory_handle.write(c.proc.worker_factories.store(std::move(factory)));
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

            const auto caller = c.proc.threads.find_handle(c.vcpu.active_thread);
            const auto reporting = std::ranges::find(factory->starting_workers, caller);
            if (reporting != factory->starting_workers.end())
            {
                factory->starting_workers.erase(reporting);
            }
            else if (!factory->starting_workers.empty())
            {
                factory->starting_workers.erase(factory->starting_workers.begin());
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                      const WORKERFACTORYINFOCLASS info_class,
                                                      const emulator_pointer worker_factory_information,
                                                      const ULONG worker_factory_information_length)
        {
            if (const auto status =
                    validate_set_information(c.win_emu, info_class, worker_factory_information, worker_factory_information_length);
                status != STATUS_SUCCESS)
            {
                return status;
            }

            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            factory->last_info_class = static_cast<ULONG>(info_class);
            factory->last_info_length = worker_factory_information_length;
            factory->last_info_value = 0;

            switch (info_class)
            {
            case WorkerFactoryStackInformation: {
                struct stack_information
                {
                    uint64_t reserve;
                    uint64_t commit;
                };

                const auto information = emulator_object<stack_information>{c.emu, worker_factory_information}.read();
                factory->stack_reserve = information.reserve;
                factory->stack_commit = information.commit;
                return STATUS_SUCCESS;
            }

            case WorkerFactoryIdleTimeout: {
                const auto value = emulator_object<LARGE_INTEGER>{c.emu, worker_factory_information}.read();
                if (value.QuadPart >= 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                factory->last_info_value = static_cast<uint64_t>(value.QuadPart);
                factory->idle_timeout = std::min(value.QuadPart, minimum_worker_idle_timeout);
                return STATUS_SUCCESS;
            }

            case WorkerFactoryBindingCount: {
                const auto raw_value = emulator_object<ULONG>{c.emu, worker_factory_information}.read();
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

            case WorkerFactoryCallbackType:
                return adjust_active_workers(c, *factory, emulator_object<ULONG>{c.emu, worker_factory_information}.read());

            case WorkerFactoryThreadMinimum:
            case WorkerFactoryThreadMaximum:
            case WorkerFactoryAdjustThreadGoal:
            case WorkerFactoryThreadBasePriority:
            case WorkerFactoryTimeoutWaiters:
            case WorkerFactoryFlags:
            case WorkerFactoryThreadSoftMaximum: {
                const auto value = emulator_object<ULONG>{c.emu, worker_factory_information}.read();
                factory->last_info_value = value;

                switch (info_class)
                {
                case WorkerFactoryThreadMinimum: {
                    // A factory being shut down keeps its limits; ntoskrnl answers STATUS_ABANDONED.
                    if (factory->shutdown)
                    {
                        return STATUS_ABANDONED;
                    }

                    // The maximum rises with the minimum, and only a raised minimum starts its workers.
                    const auto raised = value > factory->thread_minimum;
                    factory->thread_minimum = value;
                    factory->thread_maximum = std::max<uint32_t>(factory->thread_maximum, value);
                    if (raised)
                    {
                        leave_away_mode(*factory);
                        return ensure_worker_factory_threads(c, *factory);
                    }
                    break;
                }
                case WorkerFactoryThreadMaximum: {
                    if (factory->shutdown)
                    {
                        return STATUS_ABANDONED;
                    }

                    // Work waiting for a worker: a queued packet, or a release while there was no maximum.
                    const auto* completion = c.proc.get_object_as<io_completion>(factory->io_completion);
                    const auto work_queued =
                        (factory->thread_maximum == 0 && factory->pending_release_count != 0) || (completion && !completion->queue.empty());

                    // The minimum falls with the maximum.
                    factory->thread_maximum = value;
                    factory->thread_minimum = std::min<uint32_t>(factory->thread_minimum, value);

                    if (value == 0 || !work_queued || factory->start_routine == 0)
                    {
                        break;
                    }

                    // A maximum with room for it starts one worker for that work, unless one is still starting.
                    prune_worker_factory_threads(c.win_emu, *factory);
                    if (factory->pending_worker_count == 0 && count_active_and_starting_workers(*factory) < factory->thread_maximum)
                    {
                        leave_away_mode(*factory);
                        // Windows 10 19041 accepts the maximum even when worker creation fails.
                        (void)create_worker_factory_thread(c.win_emu, *factory);
                    }
                    break;
                }
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

                return STATUS_SUCCESS;
            }

            case WorkerFactoryThreadCpuSets: {
                // Each 8-byte value is the processor mask of one group (KiValidateCpuSetMasks, ntoskrnl 26100). Once the factory
                // has created its first worker the call fails for good with the state transition status.
                if (factory->shutdown)
                {
                    return STATUS_ABANDONED;
                }

                if (factory->worker_created)
                {
                    return STATUS_INVALID_STATE_TRANSITION;
                }

                const auto masks = worker_factory_information_length / sizeof(uint64_t);
                if (masks > 1)
                {
                    return STATUS_CPU_SET_INVALID;
                }

                if (masks == 1)
                {
                    const auto processors = std::min<uint32_t>(
                        c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; }), 64);
                    const auto active_processors = processors >= 64 ? ~uint64_t{0} : (uint64_t{1} << processors) - 1;
                    if ((emulator_object<uint64_t>{c.emu, worker_factory_information}.read() & ~active_processors) != 0)
                    {
                        return STATUS_CPU_SET_INVALID;
                    }
                }

                return STATUS_SUCCESS;
            }

            default:
                return STATUS_INVALID_INFO_CLASS;
            }
        }

        NTSTATUS handle_NtQueryInformationWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                        const WORKERFACTORYINFOCLASS info_class,
                                                        const emulator_pointer worker_factory_information,
                                                        const ULONG worker_factory_information_length,
                                                        const emulator_object<ULONG> return_length)
        {
            if (info_class != WorkerFactoryBasicInformation)
            {
                return STATUS_INVALID_INFO_CLASS;
            }

            using basic_information = WORKER_FACTORY_BASIC_INFORMATION<EmulatorTraits<Emu64>>;
            if (worker_factory_information % sizeof(ULONG) != 0)
            {
                return STATUS_DATATYPE_MISALIGNMENT;
            }

            if (const auto status = probe_range(c.win_emu, worker_factory_information, sizeof(basic_information), memory_permission::write);
                status != STATUS_SUCCESS)
            {
                return status;
            }

            if (return_length)
            {
                if (const auto status = probe_range(c.win_emu, return_length.value(), sizeof(ULONG), memory_permission::write);
                    status != STATUS_SUCCESS)
                {
                    return status;
                }

                return_length.write(sizeof(basic_information));
            }

            if (worker_factory_information_length != sizeof(basic_information))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            prune_worker_factory_threads(c.win_emu, *factory);

            basic_information information{};
            information.IdleTimeout.QuadPart = factory->idle_timeout;
            information.Shutdown = factory->shutdown;
            information.BindingCount = factory->binding_count;
            information.ThreadMinimum = factory->thread_minimum;
            information.ThreadMaximum = factory->thread_maximum;
            information.PendingWorkerCount = factory->pending_worker_count;
            information.WaitingWorkerCount = static_cast<ULONG>(
                count_factory_waits(c.win_emu, c.proc.worker_factories.reference_of(worker_factory_handle).value_or(object_reference{})));
            information.TotalWorkerCount = static_cast<ULONG>(count_total_workers(*factory));
            information.ReleaseCount = factory->pending_release_count;
            information.StartRoutine = factory->start_routine;
            information.StartParameter = factory->start_parameter;
            information.ProcessId = c.proc.process_id;
            information.StackReserve = factory->stack_reserve;
            information.StackCommit = factory->stack_commit;

            emulator_object<basic_information>{c.emu, worker_factory_information}.write(information);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtShutdownWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                const emulator_object<LONG> pending_worker_count)
        {
            if (pending_worker_count.value() % sizeof(LONG) != 0)
            {
                return STATUS_DATATYPE_MISALIGNMENT;
            }

            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            factory->shutdown = true;
            leave_away_mode(*factory);
            factory->thread_minimum = 0;
            factory->thread_maximum = 0;

            // Every worker that waits wakes with one empty packet; the waits after that are abandoned.
            if (const auto reference = c.proc.worker_factories.reference_of(worker_factory_handle))
            {
                worker_factory_support::request_shutdown_wake(c.win_emu, *reference);
            }

            // The starting workers are added to the caller's count (a locked add that wraps) before the factory drops them: when the
            // count cannot be reached the call fails with the starting workers still counted. Workers that reported ready stay.
            if (probe_range(c.win_emu, pending_worker_count.value(), sizeof(uint32_t), memory_permission::write) != STATUS_SUCCESS)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            const emulator_object<uint32_t> reported{c.emu, pending_worker_count.value()};
            reported.write(reported.read() + factory->pending_worker_count);

            std::erase_if(factory->worker_threads, [&](const handle thread_handle) {
                return std::ranges::find(factory->starting_workers, thread_handle) != factory->starting_workers.end();
            });
            factory->starting_workers.clear();
            factory->pending_worker_count = 0;

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

                const auto factory_reference = c.proc.worker_factories.reference_of(worker_factory_handle);
                if (!factory_reference || !worker_factory_support::enqueue_release_completion(c.win_emu, *factory_reference))
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

            if (const auto status = io_completion_wait::probe_completion_entries(c.win_emu.memory, c.win_emu.active_thread(),
                                                                                 mini_packets.value(), count, packets_returned.value());
                status != STATUS_SUCCESS)
            {
                return status;
            }

            auto* factory = c.proc.worker_factories.get(worker_factory_handle);
            if (!factory)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (factory->shutdown)
            {
                return STATUS_ABANDONED;
            }

            auto* completion = c.proc.get_object_as<io_completion>(factory->io_completion);
            if (!completion)
            {
                return STATUS_INVALID_HANDLE;
            }

            // A worker back for work takes the port's next packet itself.
            leave_away_mode(*factory);

            // IoRemoveIoCompletion's KeRemoveQueueEx: the worker joins the factory's port (KiSwitchQueue), which it stays with
            // after the call.
            c.thread().associated_io_completion = factory->io_completion;

            const auto messages = io_completion_wait::dequeue_io_completion_entries(
                c.win_emu, factory->io_completion, count,
                c.proc.worker_factories.reference_of(worker_factory_handle).value_or(object_reference{}));
            if (!messages.empty())
            {
                // A worker taking work makes sure another one is coming for whatever is still queued, or for what comes next.
                check_for_more_work(c.win_emu, *factory);
                return io_completion_wait::write_entries(c.win_emu.memory, c.win_emu.active_thread(), mini_packets.value(),
                                                         packets_returned.value(), messages);
            }

            // NtWaitForWorkViaWorkerFactory keeps the count in a local and stores it only when it succeeds (19041): nothing is
            // written before the wait, nor when it ends without work.
            auto& t = c.thread();
            t.await_objects = {};
            t.await_any = false;
            t.await_time = {};
            t.await_msg = {};
            t.waiting_for_alert = false;
            t.await_io_completion = pending_io_completion_wait{};

            auto& wait = *t.await_io_completion;
            wait.type = io_completion_wait_type::remove_multiple;
            wait.io_completion = factory->io_completion;
            wait.worker_factory = c.proc.worker_factories.reference_of(worker_factory_handle).value_or(object_reference{});
            wait.completion_entries_ptr = mini_packets.value();
            wait.entries_removed_ptr = packets_returned.value();
            wait.max_entries = count;

            arm_idle_deadline(c.win_emu, *factory, wait);

            io_completion_wait::add_waiter(*completion, t.id);
            c.win_emu.yield_thread(c.vcpu, false);
            return STATUS_SUCCESS;
        }
    }

    namespace worker_factory_support
    {
        void check_for_more_work(windows_emulator& win_emu, const object_reference& worker_factory)
        {
            if (auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(worker_factory))
            {
                syscalls::check_for_more_work(win_emu, *factory);
            }
        }

        bool end_worker_wait_by_timeout(windows_emulator& win_emu, emulator_thread& worker, pending_io_completion_wait& wait)
        {
            auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(wait.worker_factory);
            if (!factory)
            {
                return true;
            }

            syscalls::prune_worker_factory_threads(win_emu, *factory);
            const auto is_worker = [&](const handle thread_handle) { return win_emu.process.threads.get(thread_handle) == &worker; };

            const auto other_workers = std::ranges::count_if(factory->worker_threads, std::not_fn(is_worker));
            if (static_cast<size_t>(other_workers) + 1 <= factory->thread_minimum)
            {
                syscalls::arm_idle_deadline(win_emu, *factory, wait);
                return false;
            }

            std::vector<handle> departed{};
            std::erase_if(factory->worker_threads, [&](const handle thread_handle) {
                const auto leaves = is_worker(thread_handle);
                if (leaves)
                {
                    departed.push_back(thread_handle);
                }

                return leaves;
            });
            syscalls::forget_departed_workers(*factory, departed);
            syscalls::check_for_more_work(win_emu, *factory);
            return true;
        }

        namespace
        {
            void post_shutdown_packet(windows_emulator& win_emu, const object_reference& worker_factory)
            {
                auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(worker_factory);
                auto* completion = factory ? win_emu.process.get_object_as<io_completion>(factory->io_completion) : nullptr;
                if (!completion)
                {
                    return;
                }

                io_completion_message packet{};
                packet.io_status_block.Pointer = 0;
                packet.worker_factory = worker_factory;
                packet.worker_factory_shutdown = true;
                io_completion_wait::insert(win_emu, factory->io_completion, *completion, packet);
            }
        }

        void continue_shutdown_wake(windows_emulator& win_emu, const object_reference& worker_factory, const bool removed_by_waiting_worker)
        {
            auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(worker_factory);
            if (!factory)
            {
                return;
            }

            // The worker that took the packet is counted until its wait returns.
            const auto waiting = syscalls::count_factory_waits(win_emu, worker_factory) + (removed_by_waiting_worker ? 1 : 0);
            if (!factory->shutdown || waiting == 0)
            {
                factory->shutdown_wake_posted = false;
                return;
            }

            post_shutdown_packet(win_emu, worker_factory);
        }

        void request_shutdown_wake(windows_emulator& win_emu, const object_reference& worker_factory)
        {
            auto* factory = win_emu.process.get_object_as<sogen::worker_factory>(worker_factory);
            if (!factory || factory->shutdown_wake_posted || syscalls::count_factory_waits(win_emu, worker_factory) == 0)
            {
                return;
            }

            factory->shutdown_wake_posted = true;
            post_shutdown_packet(win_emu, worker_factory);
        }

        void on_io_completion_queued(windows_emulator& win_emu, const io_completion& completion)
        {
            // ponytail: walks the process's few worker factories for each queued packet; keep the waiting factories on the
            // port if a guest ever runs many pools.
            for (auto& [index, factory] : win_emu.process.worker_factories)
            {
                if (!factory.away_notification || win_emu.process.get_object_as<io_completion>(factory.io_completion) != &completion)
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
