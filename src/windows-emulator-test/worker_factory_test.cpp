#include "std_include.hpp"

#include <io_completion_wait.hpp>
#include <worker_factory_support.hpp>
#ifndef __EMSCRIPTEN__
#include <subprocess_process_manager.hpp>
#endif

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, handle worker_factory_handle, WORKERFACTORYINFOCLASS info_class,
                                                  uint64_t information, ULONG information_length);
    NTSTATUS handle_NtReleaseWorkerFactoryWorker(const syscall_context& c, handle worker_factory_handle);
    NTSTATUS handle_NtWorkerFactoryWorkerReady(const syscall_context& c, handle worker_factory_handle);
    NTSTATUS handle_NtWaitForWorkViaWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                  emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> mini_packets,
                                                  ULONG count, emulator_object<ULONG> packets_returned, emulator_pointer deferred_work);
}

namespace sogen::test
{
    namespace
    {
        class WorkerFactoryTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                worker_factory factory{};
                factory.stack_reserve = 0x100000;
                factory.stack_commit = 0x1000;
                this->factory_handle = this->emu->process.worker_factories.store(std::move(factory));
            }

            NTSTATUS set_stack(uint64_t address, ULONG size)
            {
                const syscall_context c{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtSetInformationWorkerFactory(c, this->factory_handle, WorkerFactoryStackInformation, address,
                                                                      size);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            worker_factory& pool()
            {
                return *this->emu->process.worker_factories.get(this->factory_handle);
            }

            // Shaped like the factory ntdll's TpAllocPoolInternal creates: a completion port, TppWorkerThread as the start
            // routine (never run here) and a 512-thread maximum.
            void make_pool(const uint32_t thread_maximum = 512)
            {
                this->port = this->emu->process.io_completions.store(io_completion{});
                auto& factory = this->pool();
                factory.io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{});
                factory.start_routine = 0x10000;
                factory.thread_maximum = thread_maximum;
            }

            NTSTATUS release()
            {
                return syscalls::handle_NtReleaseWorkerFactoryWorker(this->context(), this->factory_handle);
            }

            NTSTATUS worker_ready()
            {
                return syscalls::handle_NtWorkerFactoryWorkerReady(this->context(), this->factory_handle);
            }

            // A worker takes the queued release, as NtWaitForWorkViaWorkerFactory does, and then runs its callback.
            void take_release()
            {
                io_completion_message message{};
                ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->port, message));
                EXPECT_TRUE(message.worker_factory_release);
            }

            // One started worker that is now busy inside a callback.
            void start_busy_worker()
            {
                ASSERT_EQ(STATUS_SUCCESS, this->release());
                ASSERT_EQ(1u, this->pool().worker_threads.size());
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
                this->take_release();
            }

            // TpSetPoolMinThreads, TpSetPoolMaxThreads and the other ULONG settings.
            NTSTATUS set_limit(const WORKERFACTORYINFOCLASS info_class, const ULONG value)
            {
                emulator_object<ULONG>{this->emu->memory, this->storage}.write(value);
                return syscalls::handle_NtSetInformationWorkerFactory(this->context(), this->factory_handle, info_class, this->storage,
                                                                      sizeof(ULONG));
            }

            // TpSetPoolWorkerThreadIdleTimeout.
            NTSTATUS set_idle_timeout(const int64_t value)
            {
                emulator_object<int64_t>{this->emu->memory, this->storage}.write(value);
                return syscalls::handle_NtSetInformationWorkerFactory(this->context(), this->factory_handle, WorkerFactoryIdleTimeout,
                                                                      this->storage, sizeof(int64_t));
            }

            // The worker's call into NtWaitForWorkViaWorkerFactory with nothing queued: it waits.
            void wait_for_work(emulator_thread& worker)
            {
                this->emu->vcpu(0).active_thread = &worker;
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context(), this->factory_handle,
                                                                          {this->emu->memory, this->storage + 0x100}, 1,
                                                                          {this->emu->memory, this->storage + 0x80}, 0);
                }));
                ASSERT_TRUE(worker.await_io_completion.has_value());
            }

            void advance_time(const std::chrono::milliseconds duration)
            {
                auto& clock = this->emu->scheduling_clock();
                clock.continue_time_at(clock.steady_now() + duration, clock.timestamp_counter());
            }

            bool is_ready(emulator_thread& thread)
            {
                return this->emu->synchronize([&] { return thread.is_thread_ready(*this->emu); });
            }

            // TpAdjustBindingCount: a wait or a timer bound to the pool (1) or unbound from it (-1).
            NTSTATUS bind(const LONG delta)
            {
                emulator_object<LONG>{this->emu->memory, this->storage}.write(delta);
                return syscalls::handle_NtSetInformationWorkerFactory(this->context(), this->factory_handle, WorkerFactoryBindingCount,
                                                                      this->storage, sizeof(LONG));
            }

            // A bound wait's object is signaled or a bound timer is due: its packet reaches the port.
            void queue_packet()
            {
                this->emu->synchronize([&] {
                    io_completion_wait::insert(*this->emu, this->port, *this->emu->process.io_completions.get(this->port),
                                               io_completion_message{});
                });
            }

            // The pool's first worker takes the queued packet in NtWaitForWorkViaWorkerFactory without waiting for it.
            void take_packet_without_waiting()
            {
                auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
                ASSERT_NE(nullptr, worker);
                this->emu->vcpu(0).active_thread = worker;
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context(), this->factory_handle,
                                                                          {this->emu->memory, this->storage + 0x100}, 1,
                                                                          {this->emu->memory, this->storage + 0x80}, 0);
                }));
                EXPECT_FALSE(worker->await_io_completion.has_value());
                EXPECT_EQ(1u, emulator_object<ULONG>(this->emu->memory, this->storage + 0x80).read());
            }

#ifndef __EMSCRIPTEN__
            void fail_thread_allocation()
            {
                this->process_manager = std::make_unique<subprocess_process_manager>(
                    "unused", [](uint16_t, const std::string&) { return std::vector<std::string>{}; });
                this->emu->process.process_manager_ = this->process_manager.get();
            }

            void restore_thread_allocation()
            {
                this->process_manager->initialize_current_process({.process_id = this->emu->process.process_id},
                                                                  this->emu->process.initial_thread_id);
            }

            std::unique_ptr<subprocess_process_manager> process_manager{};
#endif
            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle factory_handle{};
            handle port{};
        };
    }

#ifndef __EMSCRIPTEN__
    TEST_F(WorkerFactoryTest, FailedMinimumWorkerCreationStopsAndCanRecover)
    {
        this->make_pool();
        this->fail_thread_allocation();
        EXPECT_EQ(STATUS_INSUFFICIENT_RESOURCES, this->set_limit(WorkerFactoryThreadMinimum, 2));
        EXPECT_TRUE(this->pool().worker_threads.empty());
        EXPECT_EQ(0u, this->pool().pending_worker_count);

        this->restore_thread_allocation();
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 3));
        EXPECT_EQ(3u, this->pool().worker_threads.size());
        EXPECT_EQ(3u, this->pool().pending_worker_count);
    }

    TEST_F(WorkerFactoryTest, FailedMaximumWorkerCreationCanBeRetried)
    {
        this->make_pool(0);
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        this->fail_thread_allocation();
        EXPECT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 1));
        EXPECT_TRUE(this->pool().worker_threads.empty());
        EXPECT_EQ(0u, this->pool().pending_worker_count);

        this->restore_thread_allocation();
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 1));
        EXPECT_EQ(1u, this->pool().worker_threads.size());
        EXPECT_EQ(1u, this->pool().pending_worker_count);
    }

    TEST_F(WorkerFactoryTest, FailedQueuedWorkerCreationDoesNotPreventTheNextRelease)
    {
        this->make_pool();
        this->fail_thread_allocation();
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_TRUE(this->pool().worker_threads.empty());
        EXPECT_EQ(0u, this->pool().pending_worker_count);
        this->take_release();

        this->restore_thread_allocation();
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_EQ(1u, this->pool().worker_threads.size());
        EXPECT_EQ(1u, this->pool().pending_worker_count);
    }
#endif

    TEST_F(WorkerFactoryTest, ReleaseStartsAWorkerWhenEveryWorkerIsBusy)
    {
        this->make_pool();
        this->start_busy_worker();

        // Destiny 2's orbit hang: the only worker of the global pool never returns from its callback, so the work
        // posted next needs a new worker (ExpWorkerFactoryCheckCreate).
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_EQ(2u, this->pool().worker_threads.size());
        EXPECT_EQ(1u, this->pool().pending_worker_count);
    }

    TEST_F(WorkerFactoryTest, ReleaseLeavesTheWorkToAWaitingWorker)
    {
        this->make_pool();
        this->start_busy_worker();

        auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
        ASSERT_NE(nullptr, worker);
        worker->await_io_completion = pending_io_completion_wait{
            .io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{}),
            .type = io_completion_wait_type::remove_multiple};

        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_EQ(1u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, StartsOneWorkerAtATimeUntilItReportsReady)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        this->take_release();

        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_EQ(1u, this->pool().worker_threads.size());

        EXPECT_EQ(STATUS_SUCCESS, this->worker_ready());
        EXPECT_EQ(STATUS_UNSUCCESSFUL, this->worker_ready());
    }

    TEST_F(WorkerFactoryTest, ReleaseStaysWithinTheThreadMaximum)
    {
        this->make_pool(1);
        this->start_busy_worker();

        ASSERT_EQ(STATUS_SUCCESS, this->release());
        EXPECT_EQ(1u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, WorkerHandedAReleaseStartsAnotherForTheReleaseLeft)
    {
        this->make_pool();
        this->start_busy_worker();

        // The worker is back for more work and waits on the port.
        auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
        ASSERT_NE(nullptr, worker);
        this->emu->vcpu(0).active_thread = worker;
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context(), this->factory_handle,
                                                                  {this->emu->memory, this->storage + 0x100}, 1,
                                                                  {this->emu->memory, this->storage + 0x80}, 0);
        }));
        ASSERT_TRUE(worker->await_io_completion.has_value());

        // Two releases are left when the release packet goes out again, as after another thread took one off the port: the
        // packet goes straight to the waiting worker and the other release queues behind it, with no worker free for it.
        auto& factory = this->pool();
        factory.pending_release_count = 2;
        factory.release_pending = true;
        ASSERT_TRUE(this->emu->synchronize([&] {
            return worker_factory_support::enqueue_release_completion(
                *this->emu, *this->emu->process.worker_factories.reference_of(this->factory_handle));
        }));

        EXPECT_FALSE(worker->await_io_completion.has_value());
        EXPECT_EQ(1u, this->emu->process.io_completions.get(this->port)->queue.size());
        EXPECT_EQ(2u, factory.worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, ReleasePacketStillNamesItsFactoryAfterTheReleasedHandleCloses)
    {
        this->make_pool();

        const auto duplicate = this->emu->process.worker_factories.duplicate(this->factory_handle);
        ASSERT_TRUE(duplicate);

        auto& factory = this->pool();
        factory.pending_release_count = 2;
        factory.release_pending = true;
        ASSERT_TRUE(this->emu->synchronize([&] {
            return worker_factory_support::enqueue_release_completion(
                *this->emu, *this->emu->process.worker_factories.reference_of(this->factory_handle));
        }));

        // The guest closes the handle it released through; the duplicate keeps the factory alive. The queued packet must
        // still resolve the factory by identity to take the release and queue the next one.
        ASSERT_TRUE(this->emu->process.worker_factories.erase(this->factory_handle));
        ASSERT_NE(nullptr, this->emu->process.worker_factories.get(*duplicate));

        this->take_release();

        EXPECT_EQ(1u, factory.pending_release_count);
        EXPECT_EQ(1u, this->emu->process.io_completions.get(this->port)->queue.size());
    }

    TEST_F(WorkerFactoryTest, BindingStartsNoWorkerWhileNothingIsQueued)
    {
        this->make_pool();

        // ETW's wait on the global pool during Destiny 2's process initialization: ntoskrnl's
        // ExpTryEnterWorkerFactoryAwayMode starts no worker for it, so no pool thread waits to run the executable's TLS
        // callbacks while the main thread starts the executable.
        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));
        EXPECT_TRUE(this->pool().worker_threads.empty());
        EXPECT_TRUE(this->pool().away_mode);
        EXPECT_TRUE(this->pool().away_notification);
    }

    TEST_F(WorkerFactoryTest, PacketForABoundPoolStartsAWorker)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));

        this->queue_packet();
        EXPECT_EQ(1u, this->pool().worker_threads.size());
        EXPECT_EQ(1u, this->pool().pending_worker_count);
        EXPECT_FALSE(this->pool().away_mode);
        EXPECT_FALSE(this->pool().away_notification);
    }

    TEST_F(WorkerFactoryTest, BindingStartsAWorkerForAPacketAlreadyQueued)
    {
        this->make_pool();

        // Without bindings a queued packet starts no worker by itself.
        this->queue_packet();
        EXPECT_TRUE(this->pool().worker_threads.empty());

        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));
        EXPECT_EQ(1u, this->pool().worker_threads.size());
        EXPECT_FALSE(this->pool().away_notification);
    }

    TEST_F(WorkerFactoryTest, LastBindingStopsWaitingForWork)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));
        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));
        ASSERT_EQ(STATUS_SUCCESS, this->bind(-1));
        EXPECT_TRUE(this->pool().away_notification);

        ASSERT_EQ(STATUS_SUCCESS, this->bind(-1));
        EXPECT_FALSE(this->pool().away_mode);
        EXPECT_FALSE(this->pool().away_notification);

        this->queue_packet();
        EXPECT_TRUE(this->pool().worker_threads.empty());
    }

    TEST_F(WorkerFactoryTest, BoundPoolWhoseWorkerTookWorkStartsAnotherForTheNextPacket)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->bind(1));
        this->queue_packet();
        ASSERT_EQ(1u, this->pool().worker_threads.size());
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());

        // With nothing left queued, the end of NtWaitForWorkViaWorkerFactory makes the pool wait for the next packet.
        this->take_packet_without_waiting();
        EXPECT_EQ(1u, this->pool().worker_threads.size());
        EXPECT_TRUE(this->pool().away_notification);

        // It comes while that worker is still in its callback.
        this->queue_packet();
        EXPECT_EQ(2u, this->pool().worker_threads.size());
        EXPECT_FALSE(this->pool().away_notification);
    }

    TEST_F(WorkerFactoryTest, IdleWorkersLeaveAfterTheIdleTimeoutDownToTheMinimum)
    {
        constexpr int64_t ten_seconds = -100'000'000;

        struct row
        {
            ULONG thread_minimum;
            ULONG thread_maximum;
            ULONG workers;
            ULONG busy_workers;
            bool work_midway;
            size_t retiring;
        };

        for (const auto& [thread_minimum, thread_maximum, workers, busy_workers, work_midway, retiring] :
             {row{0, 512, 1, 0, false, 1}, row{0, 1, 1, 0, false, 1}, row{0, 512, 3, 0, false, 3}, row{1, 512, 1, 0, false, 0},
              row{1, 512, 2, 0, false, 1}, row{2, 512, 3, 1, false, 1}, row{0, 512, 1, 0, true, 1}})
        {
            SCOPED_TRACE(testing::Message() << "minimum " << thread_minimum << ", maximum " << thread_maximum << ", workers " << workers
                                            << ", busy " << busy_workers << ", work midway " << work_midway);

            worker_factory factory{};
            factory.stack_reserve = 0x100000;
            factory.stack_commit = 0x1000;
            this->factory_handle = this->emu->process.worker_factories.store(std::move(factory));
            this->make_pool(thread_maximum);
            ASSERT_EQ(STATUS_SUCCESS, this->set_idle_timeout(ten_seconds));
            ASSERT_EQ(STATUS_SUCCESS, this->bind(1));

            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, workers));
            for (ULONG i = 0; i < workers; ++i)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            }
            if (thread_minimum != workers)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, thread_minimum));
            }

            const auto started = this->pool().worker_threads;
            ASSERT_EQ(workers, started.size());
            std::vector<emulator_thread*> idle_workers;
            for (const auto worker_handle : started | std::views::drop(busy_workers))
            {
                auto* worker = this->emu->process.threads.get(worker_handle);
                ASSERT_NE(nullptr, worker);
                this->wait_for_work(*worker);
                idle_workers.push_back(worker);
            }

            if (work_midway)
            {
                this->advance_time(std::chrono::seconds(6));
                this->queue_packet();
                ASSERT_FALSE(idle_workers.front()->await_io_completion.has_value());
                this->wait_for_work(*idle_workers.front());
                this->advance_time(std::chrono::seconds(6));
                EXPECT_FALSE(this->is_ready(*idle_workers.front()));
                this->advance_time(std::chrono::seconds(5));
            }
            else
            {
                this->advance_time(std::chrono::seconds(thread_minimum == 0 ? 11 : 21));
            }

            size_t retired = 0;
            for (auto* worker : idle_workers)
            {
                if (this->is_ready(*worker))
                {
                    EXPECT_EQ(STATUS_TIMEOUT, worker->pending_status);
                    worker->exit_status = STATUS_SUCCESS;
                    ++retired;
                }
                else
                {
                    EXPECT_TRUE(worker->await_io_completion.has_value());
                }
            }

            EXPECT_EQ(retiring, retired);
            ASSERT_EQ(workers - retired, this->pool().worker_threads.size());

            if (this->pool().worker_threads.empty())
            {
                this->queue_packet();
                ASSERT_EQ(1u, this->pool().worker_threads.size());
                EXPECT_EQ(std::ranges::end(started), std::ranges::find(started, this->pool().worker_threads.front()));
            }
        }
    }

    TEST_F(WorkerFactoryTest, ThreadMinimumRaisesTheMaximumAndStartsItsWorkers)
    {
        this->make_pool(2);

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 4));
        EXPECT_EQ(4u, this->pool().thread_minimum);
        EXPECT_EQ(4u, this->pool().thread_maximum);
        EXPECT_EQ(4u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, ThreadMaximumLowersTheMinimumAndKeepsRunningWorkers)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 3));

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 1));
        EXPECT_EQ(1u, this->pool().thread_minimum);
        EXPECT_EQ(1u, this->pool().thread_maximum);
        EXPECT_EQ(3u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, OnlyARaisedMinimumStartsWorkers)
    {
        this->make_pool();
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        ASSERT_EQ(2u, this->pool().worker_threads.size());

        // A worker is gone: neither the same minimum again nor another setting starts one in its place.
        this->emu->process.threads.get(this->pool().worker_threads.front())->exit_status = STATUS_SUCCESS;
        const auto workers = this->pool().worker_threads;
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 8));
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadBasePriority, 1));
        EXPECT_EQ(workers, this->pool().worker_threads);

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 3));
        EXPECT_EQ(3u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, FactoryBeingShutDownKeepsItsLimits)
    {
        this->make_pool(4);
        this->pool().shutdown = true;

        EXPECT_EQ(STATUS_ABANDONED, this->set_limit(WorkerFactoryThreadMinimum, 2));
        EXPECT_EQ(STATUS_ABANDONED, this->set_limit(WorkerFactoryThreadMaximum, 1));
        EXPECT_EQ(0u, this->pool().thread_minimum);
        EXPECT_EQ(4u, this->pool().thread_maximum);
        EXPECT_TRUE(this->pool().worker_threads.empty());
    }

    TEST_F(WorkerFactoryTest, NewMaximumStartsAWorkerForQueuedWork)
    {
        this->make_pool(1);
        this->start_busy_worker();

        // A packet comes while the pool's only worker is busy and the pool is at its maximum.
        this->queue_packet();
        ASSERT_EQ(1u, this->pool().worker_threads.size());

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 2));
        EXPECT_EQ(2u, this->pool().worker_threads.size());
        EXPECT_EQ(1u, this->pool().pending_worker_count);
    }

    TEST_F(WorkerFactoryTest, NewMaximumStartsAWorkerForAReleaseThatHadNone)
    {
        this->make_pool(0);
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        ASSERT_TRUE(this->pool().worker_threads.empty());

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 1));
        EXPECT_EQ(1u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, NewMaximumLeavesQueuedWorkToAStartingWorker)
    {
        this->make_pool(1);
        ASSERT_EQ(STATUS_SUCCESS, this->release());
        ASSERT_EQ(1u, this->pool().pending_worker_count);
        this->queue_packet();

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 4));
        EXPECT_EQ(1u, this->pool().worker_threads.size());
    }

    TEST_F(WorkerFactoryTest, UpdatesStackSizesIncludingZeroAndUnalignedSizes)
    {
        for (const auto sizes : {std::array<uint64_t, 2>{0x200000, 0x2000}, std::array<uint64_t, 2>{0, 0},
                                 std::array<uint64_t, 2>{0x1000, 0x2000}, std::array<uint64_t, 2>{1, 1}})
        {
            emulator_object<std::array<uint64_t, 2>>{this->emu->memory, this->storage}.write(sizes);
            ASSERT_EQ(STATUS_SUCCESS, this->set_stack(this->storage, sizeof(sizes)));
            const auto* factory = this->emu->process.worker_factories.get(this->factory_handle);
            ASSERT_NE(nullptr, factory);
            EXPECT_EQ(sizes[0], factory->stack_reserve);
            EXPECT_EQ(sizes[1], factory->stack_commit);
        }
    }

    TEST_F(WorkerFactoryTest, RejectsWrongLengthAndUnreadableBufferWithoutChangingStackSizes)
    {
        for (const ULONG length : {0u, 8u, 15u, 24u})
        {
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->set_stack(this->storage, length));
        }
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->set_stack(0x12345678, 16));
        const auto* factory = this->emu->process.worker_factories.get(this->factory_handle);
        ASSERT_NE(nullptr, factory);
        EXPECT_EQ(0x100000u, factory->stack_reserve);
        EXPECT_EQ(0x1000u, factory->stack_commit);
    }
}
