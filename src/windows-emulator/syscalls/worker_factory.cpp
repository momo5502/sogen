#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../io_completion_wait.hpp"
#include "../syscall_utils.hpp"

#include <limits>

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

        void prune_worker_factory_threads(const syscall_context& c, worker_factory& factory)
        {
            std::erase_if(factory.worker_threads, [&](const handle thread_handle) {
                const auto* thread = c.proc.threads.get(thread_handle);
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

        void ensure_worker_factory_threads(const syscall_context& c, worker_factory& factory)
        {
            if (factory.shutdown || factory.paused != 0 || factory.start_routine == 0)
            {
                return;
            }

            prune_worker_factory_threads(c, factory);

            const auto limit = get_worker_factory_thread_limit(factory);
            if (limit == 0)
            {
                return;
            }

            auto desired = std::max(factory.thread_minimum, factory.binding_count);
            desired = std::min(desired, limit);

            while (factory.worker_threads.size() < desired)
            {
                const auto stack_size =
                    factory.stack_reserve != 0 ? factory.stack_reserve : c.win_emu.mod_manager.executable->size_of_stack_reserve;
                const auto create_flags = (factory.flags & WORKER_FACTORY_FLAG_LOADER_POOL) ? THREAD_CREATE_FLAGS_LOADER_WORKER : 0;
                const auto thread_handle =
                    c.proc.create_thread(c.win_emu.memory, factory.start_routine, factory.start_parameter, stack_size, create_flags);
                factory.worker_threads.push_back(thread_handle);
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

        if (worker_process_handle != CURRENT_PROCESS)
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

        handle retained_io_completion_handle{};
        if (!io_completion_wait::retain_handle_reference(c.proc, io_completion_handle, retained_io_completion_handle))
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

        ensure_worker_factory_threads(c, *factory);
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

            ensure_worker_factory_threads(c, *factory);
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
                factory->thread_minimum = value;
                break;
            case WorkerFactoryThreadMaximum:
                factory->thread_maximum = value;
                break;
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

        ensure_worker_factory_threads(c, *factory);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtWaitForWorkViaWorkerFactory(const syscall_context& c, const handle worker_factory_handle,
                                                  const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> mini_packets,
                                                  const ULONG count, const emulator_object<ULONG> packets_returned,
                                                  const emulator_pointer /*deferred_work*/)
    {
        if (count == 0 || !mini_packets)
        {
            return STATUS_INVALID_PARAMETER;
        }

        auto* factory = c.proc.worker_factories.get(worker_factory_handle);
        if (!factory)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (!c.proc.io_completions.get(factory->io_completion_handle))
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto removed = io_completion_wait::dequeue_io_completion_entries(c.proc, factory->io_completion_handle, mini_packets, count);
        packets_returned.write_if_valid(removed);
        if (removed > 0)
        {
            return STATUS_SUCCESS;
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
        wait.io_completion_handle = factory->io_completion_handle;
        wait.completion_entries_ptr = mini_packets.value();
        wait.entries_removed_ptr = packets_returned.value();
        wait.max_entries = count;

        if (factory->timeout != 0)
        {
            LARGE_INTEGER timeout{};
            timeout.QuadPart = factory->timeout;
            wait.timeout = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout);

            constexpr auto infinite = std::chrono::steady_clock::time_point::min();
            if (wait.timeout.has_value() && wait.timeout.value() != infinite && wait.timeout.value() < c.win_emu.clock().steady_now())
            {
                packets_returned.write_if_valid(0);
                t.await_io_completion = {};
                return STATUS_TIMEOUT;
            }
        }

        c.win_emu.yield_thread(false);
        return STATUS_SUCCESS;
    }
}
