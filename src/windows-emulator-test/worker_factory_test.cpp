#include "std_include.hpp"

#include <numeric>

#include <io_completion_wait.hpp>
#include <worker_factory_support.hpp>
#ifndef __EMSCRIPTEN__
#include <subprocess_process_manager.hpp>
#endif

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationWorkerFactory(const syscall_context& c, handle worker_factory_handle, WORKERFACTORYINFOCLASS info_class,
                                                  uint64_t information, ULONG information_length);
    NTSTATUS handle_NtQueryInformationWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                    WORKERFACTORYINFOCLASS info_class, uint64_t information, ULONG information_length,
                                                    emulator_object<ULONG> return_length);
    NTSTATUS handle_NtRemoveIoCompletionEx(const syscall_context& c, handle io_completion_handle,
                                           emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information,
                                           ULONG count, emulator_object<ULONG> num_entries_removed, emulator_object<LARGE_INTEGER> timeout,
                                           BOOLEAN alertable);
    NTSTATUS handle_NtReleaseWorkerFactoryWorker(const syscall_context& c, handle worker_factory_handle);
    NTSTATUS handle_NtShutdownWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                            emulator_object<LONG> pending_worker_count);
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
                this->new_factory();
            }

            // A test with many rows takes a fresh factory for each one on its emulator: an emulator costs a lot of memory, which the
            // Emscripten build does not have for more than a few at a time.
            void new_factory()
            {
                worker_factory factory{};
                factory.stack_reserve = 0x100000;
                factory.stack_commit = 0x1000;
                this->factory_handle = this->emu->process.worker_factories.store(std::move(factory));
            }

            emulator_thread* new_outsider()
            {
                const auto handle = this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, ++this->next_outsider_id, true});
                return this->emu->process.threads.get(handle);
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

            NTSTATUS set_information(const ULONG info_class, const ULONG length, const bool null_buffer, const bool invalid_handle = false)
            {
                if (length == sizeof(int64_t))
                {
                    emulator_object<int64_t>{this->emu->memory, this->storage}.write(-20'000'000);
                }
                else
                {
                    emulator_object<ULONG>{this->emu->memory, this->storage}.write(3);
                }
                return syscalls::handle_NtSetInformationWorkerFactory(this->context(), invalid_handle ? handle{} : this->factory_handle,
                                                                      static_cast<WORKERFACTORYINFOCLASS>(info_class),
                                                                      null_buffer ? 0 : this->storage, length);
            }

            NTSTATUS set_callback_type(const ULONG value, const ULONG length = sizeof(ULONG))
            {
                emulator_object<ULONG>{this->emu->memory, this->storage}.write(value);
                return syscalls::handle_NtSetInformationWorkerFactory(this->context(), this->factory_handle, WorkerFactoryCallbackType,
                                                                      this->storage, length);
            }

            NTSTATUS query_information(const ULONG info_class, const ULONG length, const uint64_t buffer, const uint64_t return_length,
                                       const bool invalid_handle = false)
            {
                return syscalls::handle_NtQueryInformationWorkerFactory(this->context(), invalid_handle ? handle{} : this->factory_handle,
                                                                        static_cast<WORKERFACTORYINFOCLASS>(info_class), buffer, length,
                                                                        {this->emu->memory, return_length});
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
            uint32_t next_outsider_id{0x7000};
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

        // A worker that left before it reported ready still counts as starting (Windows 11 26300 keeps the count): the raised
        // minimum of 3 finds two starting workers and starts one more.
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 3));
        EXPECT_EQ(2u, this->pool().worker_threads.size());
        EXPECT_EQ(3u, this->pool().pending_worker_count);
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

    // Windows 11 26300 (x64) on a factory without workers, given -20000000 (length 8) or 3 (length 4).
    TEST_F(WorkerFactoryTest, SetInformationAnswersEveryClassLengthAndBufferLikeWindows)
    {
        struct row
        {
            ULONG info_class;
            ULONG length;
            bool null_buffer;
            NTSTATUS status;
        };

        const std::vector<row> rows{
            {0, 8, false, STATUS_INVALID_INFO_CLASS},
            {0, 4, false, STATUS_INVALID_INFO_CLASS},
            {0, 0, true, STATUS_INVALID_INFO_CLASS},
            {0, 8, true, STATUS_INVALID_INFO_CLASS},
            {1, 8, false, STATUS_INVALID_INFO_CLASS},
            {1, 4, false, STATUS_INVALID_INFO_CLASS},
            {2, 8, false, STATUS_SUCCESS},
            {2, 4, false, STATUS_INFO_LENGTH_MISMATCH},
            {2, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {2, 8, true, STATUS_ACCESS_VIOLATION},
            {3, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {3, 4, false, STATUS_SUCCESS},
            {3, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {3, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {4, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {4, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {5, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {5, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {6, 8, false, STATUS_NOT_IMPLEMENTED},
            {6, 4, false, STATUS_NOT_IMPLEMENTED},
            {6, 0, true, STATUS_NOT_IMPLEMENTED},
            {6, 8, true, STATUS_NOT_IMPLEMENTED},
            {7, 8, false, STATUS_INVALID_INFO_CLASS},
            {7, 4, false, STATUS_INVALID_INFO_CLASS},
            {7, 0, true, STATUS_INVALID_INFO_CLASS},
            {8, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {8, 4, false, STATUS_SUCCESS},
            {8, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {8, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {10, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {10, 4, false, STATUS_INFO_LENGTH_MISMATCH},
            {10, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {10, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {11, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {11, 4, false, STATUS_SUCCESS},
            {11, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {12, 4, false, STATUS_SUCCESS},
            {12, 0, true, STATUS_INFO_LENGTH_MISMATCH},
            {13, 4, false, STATUS_SUCCESS},
            {13, 8, false, STATUS_INFO_LENGTH_MISMATCH},
            {14, 4, false, STATUS_SUCCESS},
            {14, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {16, 8, false, STATUS_INVALID_INFO_CLASS},
            {16, 0, true, STATUS_INVALID_INFO_CLASS},
            {15, 0, false, STATUS_SUCCESS},
            {15, 16, false, STATUS_CPU_SET_INVALID},
            {15, 24, false, STATUS_CPU_SET_INVALID},
            {15, 32, false, STATUS_CPU_SET_INVALID},
            {15, 0xF0, false, STATUS_CPU_SET_INVALID},
            {15, 0xF8, false, STATUS_CPU_SET_INVALID},
            {15, 0x100, false, STATUS_CPU_SET_INVALID},
            {15, 0xFC, false, STATUS_INFO_LENGTH_MISMATCH},
            {15, 0xF8, true, STATUS_ACCESS_VIOLATION},
            {15, 0x108, false, STATUS_INFO_LENGTH_MISMATCH},
            {15, 0x108, true, STATUS_INFO_LENGTH_MISMATCH},
            {15, 4, false, STATUS_INFO_LENGTH_MISMATCH},
            {15, 12, false, STATUS_INFO_LENGTH_MISMATCH},
            {15, 1, true, STATUS_INFO_LENGTH_MISMATCH},
            {15, 8, true, STATUS_ACCESS_VIOLATION},
            {15, 32, true, STATUS_ACCESS_VIOLATION},
            {20, 4, false, STATUS_INVALID_INFO_CLASS},
            {21, 8, false, STATUS_INVALID_INFO_CLASS},
            {0xFFFFFFFF, 8, false, STATUS_INVALID_INFO_CLASS},
        };

        for (const auto& [info_class, length, null_buffer, status] : rows)
        {
            EXPECT_EQ(status, this->set_information(info_class, length, null_buffer))
                << "class " << info_class << " length " << length << (null_buffer ? " null buffer" : "");
        }

        // ntoskrnl looks at the class, the length and the buffer before it looks at the handle.
        const std::vector<row> invalid_handle_rows{
            {0, 8, false, STATUS_INVALID_INFO_CLASS},       {6, 8, false, STATUS_NOT_IMPLEMENTED},
            {7, 8, false, STATUS_INVALID_INFO_CLASS},       {16, 8, false, STATUS_INVALID_INFO_CLASS},
            {2, 4, false, STATUS_INFO_LENGTH_MISMATCH},     {3, 8, true, STATUS_INFO_LENGTH_MISMATCH},
            {2, 8, true, STATUS_ACCESS_VIOLATION},          {15, 0x108, false, STATUS_INFO_LENGTH_MISMATCH},
            {2, 8, false, STATUS_INVALID_HANDLE},           {9, 4, false, STATUS_INVALID_HANDLE},
            {15, 8, false, STATUS_INVALID_HANDLE},          {15, 0xF8, false, STATUS_INVALID_HANDLE},
            {15, 0x100, false, STATUS_INVALID_HANDLE},      {15, 0, true, STATUS_INVALID_HANDLE},
            {15, 0xFC, false, STATUS_INFO_LENGTH_MISMATCH},
        };

        for (const auto& [info_class, length, null_buffer, status] : invalid_handle_rows)
        {
            EXPECT_EQ(status, this->set_information(info_class, length, null_buffer, true))
                << "invalid handle, class " << info_class << " length " << length << (null_buffer ? " null buffer" : "");
        }

        // A factory with a worker, started or starting, answers class 15 with the state transition status.
        this->make_pool(4);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
        for (const ULONG length : {0u, 8u, 16u, 32u, 0xF8u})
        {
            EXPECT_EQ(STATUS_INVALID_STATE_TRANSITION, this->set_information(15, length, length == 0)) << "length " << length;
        }
    }

    // Windows 11: class 9 moves the count of active workers. Type 0 counts a worker active again, type 1 asks for a worker
    // (STATUS_TOO_MANY_THREADS at the maximum), type 2 takes a worker out of the count and starts one only when none is left,
    // and type 3 also takes it out of the total.
    TEST_F(WorkerFactoryTest, CallbackTypeMovesTheActiveWorkerCountLikeWindows)
    {
        this->make_pool(2);
        const auto workers = [&] { return this->pool().worker_threads.size(); };
        const auto pending = [&] { return this->pool().pending_worker_count; };
        const auto counted_workers = [&] {
            EXPECT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage, 0));
            return emulator_object<ULONG>{this->emu->memory, this->storage + 0x34}.read();
        };

        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->set_callback_type(1, 0));
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH, this->set_callback_type(1, sizeof(uint64_t)));
        for (const ULONG value : {4u, 5u, 8u, 0xFFFFFFFFu})
        {
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->set_callback_type(value));
        }
        for (const ULONG value : {0u, 2u, 3u})
        {
            EXPECT_EQ(STATUS_UNSUCCESSFUL, this->set_callback_type(value));
        }
        EXPECT_EQ(0u, workers());

        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(1));
        EXPECT_EQ(1u, workers());
        EXPECT_EQ(1u, pending());
        EXPECT_EQ(0u, counted_workers());
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        EXPECT_EQ(1u, counted_workers());
        EXPECT_EQ(STATUS_UNSUCCESSFUL, this->set_callback_type(0));

        // One ready worker and one starting reach the maximum of 2: no third worker, and no error.
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(1));
        EXPECT_EQ(2u, workers());
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(1));
        EXPECT_EQ(2u, workers());
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        EXPECT_EQ(STATUS_TOO_MANY_THREADS, this->set_callback_type(1));

        // Two active workers: the first type 2 starts nothing, the second finds none active.
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));
        EXPECT_EQ(2u, workers());
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));
        EXPECT_EQ(3u, workers());
        EXPECT_EQ(1u, pending());
        EXPECT_EQ(STATUS_UNSUCCESSFUL, this->set_callback_type(2));
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        EXPECT_EQ(3u, counted_workers());

        // Type 0 puts the two workers back, then every worker is active.
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(0));
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(0));
        EXPECT_EQ(STATUS_UNSUCCESSFUL, this->set_callback_type(0));

        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(3));
        EXPECT_EQ(2u, counted_workers());
        EXPECT_EQ(3u, workers());
        EXPECT_EQ(0u, pending());
    }

    // Windows 11 26300: class 9 called from a thread outside a pool of N idle workers, N in the first column. "settle" means
    // the workers started by earlier steps have reported ready before the call; pending and total are read after the call.
    TEST_F(WorkerFactoryTest, CallbackTypeFollowsTheCapturedPoolSequences)
    {
        struct step
        {
            ULONG type;
            bool settle;
            NTSTATUS status;
            ULONG pending;
            ULONG total;
        };

        struct sequence
        {
            ULONG workers;
            ULONG maximum;
            std::vector<step> steps;
            ULONG settled_total;
        };

        const std::vector<sequence> sequences{
            {2, 4, {{2, false, STATUS_SUCCESS, 0, 2}, {2, false, STATUS_SUCCESS, 1, 2}, {2, true, STATUS_SUCCESS, 1, 3}}, 4},
            {2, 4, {{2, false, STATUS_SUCCESS, 0, 2}, {2, false, STATUS_SUCCESS, 1, 2}, {2, false, STATUS_UNSUCCESSFUL, 1, 2}}, 3},
            {2, 4, {{2, false, STATUS_SUCCESS, 0, 2}, {0, false, STATUS_SUCCESS, 0, 2}, {0, false, STATUS_UNSUCCESSFUL, 0, 2}}, 2},
            {1, 4, {{2, false, STATUS_SUCCESS, 1, 1}, {0, true, STATUS_SUCCESS, 0, 2}, {0, false, STATUS_UNSUCCESSFUL, 0, 2}}, 2},
            {2, 4, {{3, false, STATUS_SUCCESS, 0, 1}, {3, false, STATUS_SUCCESS, 1, 0}, {3, false, STATUS_UNSUCCESSFUL, 1, 0}}, 1},
            {2, 4, {{3, false, STATUS_SUCCESS, 0, 1}, {0, false, STATUS_UNSUCCESSFUL, 0, 1}, {0, false, STATUS_UNSUCCESSFUL, 0, 1}}, 1},
            {3, 4, {{3, false, STATUS_SUCCESS, 0, 2}, {2, false, STATUS_SUCCESS, 0, 2}, {0, false, STATUS_SUCCESS, 0, 2}}, 2},
            {1, 2, {{1, false, STATUS_SUCCESS, 1, 1}, {1, true, STATUS_TOO_MANY_THREADS, 0, 2}}, 2},
            {1, 2, {{1, false, STATUS_SUCCESS, 1, 1}, {1, false, STATUS_SUCCESS, 1, 1}, {1, true, STATUS_TOO_MANY_THREADS, 0, 2}}, 2},
            {2, 2, {{1, false, STATUS_TOO_MANY_THREADS, 0, 2}, {1, false, STATUS_TOO_MANY_THREADS, 0, 2}}, 2},
            {1, 1, {{1, false, STATUS_TOO_MANY_THREADS, 0, 1}}, 1},
        };

        const auto total_workers = [&] {
            EXPECT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage, 0));
            return emulator_object<ULONG>{this->emu->memory, this->storage + 0x34}.read();
        };

        for (const auto& [workers, maximum, steps, settled_total] : sequences)
        {
            this->new_factory();
            this->make_pool(maximum);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, workers));
            while (this->pool().pending_worker_count != 0)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            }

            for (const auto& [type, settle, status, pending, total] : steps)
            {
                while (settle && this->pool().pending_worker_count != 0)
                {
                    ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
                }

                SCOPED_TRACE(testing::Message() << "workers " << workers << " maximum " << maximum << " type " << type);
                EXPECT_EQ(status, this->set_callback_type(type));
                EXPECT_EQ(pending, this->pool().pending_worker_count);
                EXPECT_EQ(total, total_workers());
            }

            while (this->pool().pending_worker_count != 0)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            }
            EXPECT_EQ(settled_total, total_workers());
        }
    }

    // A type 2 or 3 that leaves no active worker goes through the checks of type 1: it starts nothing while a worker is still
    // starting, and a factory being shut down answers STATUS_ABANDONED instead of starting one (ntoskrnl 10.0.26100.9974).
    TEST_F(WorkerFactoryTest, CallbackTypeStartsNoWorkerWhileOneIsStartingOrTheFactoryIsShutDown)
    {
        for (const ULONG type : {2u, 3u})
        {
            this->new_factory();
            this->make_pool(4);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());

            ASSERT_EQ(STATUS_SUCCESS, this->set_callback_type(1));
            ASSERT_EQ(2u, this->pool().worker_threads.size());
            EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(type));
            EXPECT_EQ(2u, this->pool().worker_threads.size());
            EXPECT_EQ(1u, this->pool().pending_worker_count);
        }

        for (const ULONG type : {1u, 2u, 3u})
        {
            this->new_factory();
            this->make_pool(4);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            this->pool().shutdown = true;

            EXPECT_EQ(STATUS_ABANDONED, this->set_callback_type(type)) << "type " << type;
            EXPECT_EQ(1u, this->pool().worker_threads.size());
        }

        this->new_factory();
        this->make_pool(4);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        for (ULONG i = 0; i < 2; ++i)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        }
        this->pool().shutdown = true;
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));

        // The shutdown comes before the maximum and before a worker that is still starting.
        this->new_factory();
        this->make_pool(1);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        EXPECT_EQ(STATUS_TOO_MANY_THREADS, this->set_callback_type(1));
        this->pool().shutdown = true;
        EXPECT_EQ(STATUS_ABANDONED, this->set_callback_type(1));

        for (const ULONG type : {1u, 2u, 3u})
        {
            this->new_factory();
            this->make_pool(4);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            ASSERT_EQ(STATUS_SUCCESS, this->set_callback_type(1));
            ASSERT_EQ(1u, this->pool().pending_worker_count);
            this->pool().shutdown = true;
            EXPECT_EQ(STATUS_ABANDONED, this->set_callback_type(type)) << "type " << type;
        }
    }

    // The thread minimum counts active and starting workers, not every thread: two workers that left the active count make room
    // for another one (ntoskrnl 10.0.26100.9974).
    TEST_F(WorkerFactoryTest, ThreadMinimumCountsTheActiveWorkers)
    {
        this->make_pool(2);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        for (ULONG i = 0; i < 2; ++i)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        }
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 0));

        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));
        ASSERT_EQ(3u, this->pool().worker_threads.size());
        ASSERT_EQ(1u, this->pool().pending_worker_count);

        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        EXPECT_EQ(4u, this->pool().worker_threads.size());
        EXPECT_EQ(2u, this->pool().pending_worker_count);
    }

    // Type 3 takes the calling worker out of the total; another worker leaving the factory takes only its own share.
    TEST_F(WorkerFactoryTest, CallbackType3IsNotUndoneByAnotherWorkerLeaving)
    {
        constexpr int64_t ten_seconds = -100'000'000;
        this->make_pool(4);
        ASSERT_EQ(STATUS_SUCCESS, this->set_idle_timeout(ten_seconds));
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        for (ULONG i = 0; i < 2; ++i)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
        }
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 0));

        const auto started = this->pool().worker_threads;
        auto* first = this->emu->process.threads.get(started[0]);
        auto* second = this->emu->process.threads.get(started[1]);
        ASSERT_NE(nullptr, first);
        ASSERT_NE(nullptr, second);

        const auto total_workers = [&] {
            EXPECT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage, 0));
            return emulator_object<ULONG>{this->emu->memory, this->storage + 0x34}.read();
        };

        this->emu->vcpu(0).active_thread = first;
        ASSERT_EQ(STATUS_SUCCESS, this->set_callback_type(3));
        EXPECT_EQ(1u, total_workers());

        this->wait_for_work(*second);
        this->advance_time(std::chrono::seconds(11));
        ASSERT_TRUE(this->is_ready(*second));
        second->exit_status = STATUS_SUCCESS;
        EXPECT_EQ(0u, total_workers());

        first->exit_status = STATUS_SUCCESS;
        EXPECT_EQ(0u, total_workers());
        EXPECT_EQ(0u, this->pool().removed_worker_count);
    }

    TEST_F(WorkerFactoryTest, ThreadCpuSetsFollowTheCapturedPayloads)
    {
        struct row
        {
            std::vector<uint64_t> ids;
            NTSTATUS status;
        };

        const auto set_cpu_sets = [&](const std::vector<uint64_t>& ids) {
            for (size_t i = 0; i < ids.size(); ++i)
            {
                emulator_object<uint64_t>{this->emu->memory, this->storage + i * sizeof(uint64_t)}.write(ids[i]);
            }

            return syscalls::handle_NtSetInformationWorkerFactory(this->context(), this->factory_handle, WorkerFactoryThreadCpuSets,
                                                                  this->storage, static_cast<ULONG>(ids.size() * sizeof(uint64_t)));
        };

        // The captured rows come from a machine with one group of 16 active processors.
        this->emu->process.kusd.access([](KUSER_SHARED_DATA64& kusd) { kusd.ActiveProcessorCount = 16; });

        // Windows 11 26300, a factory without a worker. A successful set leaves the next answer alone.
        std::vector<uint64_t> ascending(16);
        std::iota(ascending.begin(), ascending.end(), uint64_t{0x100});
        const std::vector<row> rows{
            {{}, STATUS_SUCCESS},
            {{0x100}, STATUS_SUCCESS},
            {{0}, STATUS_SUCCESS},
            {{1}, STATUS_SUCCESS},
            {{0x10F}, STATUS_SUCCESS},
            {{0x110}, STATUS_SUCCESS},
            {{0x1000}, STATUS_SUCCESS},
            {{0x3}, STATUS_SUCCESS},
            {{0x7FFF}, STATUS_SUCCESS},
            {{0x18000}, STATUS_CPU_SET_INVALID},
            {{~uint64_t{0}}, STATUS_CPU_SET_INVALID},
            {{0x2000}, STATUS_SUCCESS},
            {{0x4000}, STATUS_SUCCESS},
            {{0x8000}, STATUS_SUCCESS},
            {{0xFFFF}, STATUS_SUCCESS},
            {{0x10000}, STATUS_CPU_SET_INVALID},
            {{0x20000}, STATUS_CPU_SET_INVALID},
            {{0x100000}, STATUS_CPU_SET_INVALID},
            {{0x1000000}, STATUS_CPU_SET_INVALID},
            {{0x7FFFFFFF}, STATUS_CPU_SET_INVALID},
            {{0x80000000}, STATUS_CPU_SET_INVALID},
            {{0xBFFFFFFF}, STATUS_CPU_SET_INVALID},
            {{0xFFFFFFFF}, STATUS_CPU_SET_INVALID},
            {{0x100000000ULL}, STATUS_CPU_SET_INVALID},
            {{0x1000000000000ULL}, STATUS_CPU_SET_INVALID},
            {{0xDEADBEEF}, STATUS_CPU_SET_INVALID},
            {{(0x101ULL << 32) | 0x100}, STATUS_CPU_SET_INVALID},
            {{0x100, 0x101}, STATUS_CPU_SET_INVALID},
            {{0x100, 0x100}, STATUS_CPU_SET_INVALID},
            {{0x101, 0x100}, STATUS_CPU_SET_INVALID},
            {{0x100, 0xDEADBEEF}, STATUS_CPU_SET_INVALID},
            {{0xDEADBEEF, 0x100}, STATUS_CPU_SET_INVALID},
            {{0, 0x100}, STATUS_CPU_SET_INVALID},
            {{0x100, 0}, STATUS_CPU_SET_INVALID},
            {{1, 0x100}, STATUS_CPU_SET_INVALID},
            {{2, 0x100, 0x101}, STATUS_CPU_SET_INVALID},
            {{0x100, 0x100, 0x100}, STATUS_CPU_SET_INVALID},
            {ascending, STATUS_CPU_SET_INVALID},
            {{0x100}, STATUS_SUCCESS},
        };

        for (const auto& [ids, status] : rows)
        {
            EXPECT_EQ(status, set_cpu_sets(ids)) << ids.size() << " ids, first " << (ids.empty() ? 0 : ids.front());
        }

        // Each value is the processor mask of one group (ntoskrnl 10.0.26100.9974): the captured rows above hold for 16 active
        // processors, and the same values change their answer with another processor count.
        struct mask_row
        {
            uint32_t processors;
            uint64_t mask;
            NTSTATUS status;
        };

        for (const auto& [processors, mask, status] : std::vector<mask_row>{{1, 1, STATUS_SUCCESS},
                                                                            {1, 3, STATUS_CPU_SET_INVALID},
                                                                            {8, 0xFF, STATUS_SUCCESS},
                                                                            {8, 0x100, STATUS_CPU_SET_INVALID},
                                                                            {8, 0xFFFF, STATUS_CPU_SET_INVALID},
                                                                            {16, 0x8000, STATUS_SUCCESS},
                                                                            {32, 0x10000, STATUS_SUCCESS},
                                                                            {32, 0xFFFFFFFF, STATUS_SUCCESS},
                                                                            {32, 0x100000000, STATUS_CPU_SET_INVALID},
                                                                            {64, ~uint64_t{0}, STATUS_SUCCESS}})
        {
            this->emu->process.kusd.access([&](KUSER_SHARED_DATA64& kusd) { kusd.ActiveProcessorCount = processors; });
            EXPECT_EQ(status, set_cpu_sets({mask})) << processors << " processors, mask " << mask;
        }
        this->emu->process.kusd.access([](KUSER_SHARED_DATA64& kusd) { kusd.ActiveProcessorCount = 16; });

        // A factory being shut down answers before it looks at the ids.
        this->pool().shutdown = true;
        EXPECT_EQ(STATUS_ABANDONED, set_cpu_sets({}));
        EXPECT_EQ(STATUS_ABANDONED, set_cpu_sets({0xDEADBEEF}));
        this->pool().shutdown = false;

        // Windows 11 26300, a factory with a worker that is starting, and then ready.
        this->make_pool(4);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
        for (int phase = 0; phase < 2; ++phase)
        {
            for (const auto& ids : {std::vector<uint64_t>{}, std::vector<uint64_t>{0x100}, std::vector<uint64_t>{0x100, 0x101}, ascending})
            {
                EXPECT_EQ(STATUS_INVALID_STATE_TRANSITION, set_cpu_sets(ids)) << ids.size() << " ids, phase " << phase;
            }

            if (phase == 0)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            }
        }

        // The state is permanent: the factory's workers are all gone and the call still fails (ntoskrnl 10.0.26100.9974).
        for (const auto worker_handle : this->pool().worker_threads)
        {
            this->emu->process.threads.get(worker_handle)->exit_status = STATUS_SUCCESS;
        }
        EXPECT_EQ(STATUS_INVALID_STATE_TRANSITION, set_cpu_sets({0x1}));
        EXPECT_TRUE(this->pool().worker_created);

        this->pool().shutdown = true;
        EXPECT_EQ(STATUS_ABANDONED, set_cpu_sets({0x1}));
    }

    // Windows 11 26300 after NtShutdownWorkerFactory, in the order the probe called: the class 9 types, then classes 4, 5 and 15.
    TEST_F(WorkerFactoryTest, ShutDownFactoryAnswersLikeWindows)
    {
        struct variant
        {
            ULONG maximum;
            ULONG minimum;
            ULONG ready;
            LONG reported_pending;
            ULONG total;
            std::array<NTSTATUS, 4> callback_types;
        };

        constexpr NTSTATUS failed = STATUS_UNSUCCESSFUL;
        constexpr NTSTATUS abandoned = STATUS_ABANDONED;
        const std::vector<variant> variants{
            {8, 0, 0, -1, 0, {failed, abandoned, failed, failed}},
            {8, 1, 0, 0, 0, {failed, abandoned, failed, failed}},
            {1, 1, 1, -1, 1, {failed, abandoned, abandoned, failed}},
        };

        const auto field = [&](const uint64_t offset) { return emulator_object<ULONG>{this->emu->memory, this->storage + offset}.read(); };
        for (const auto& [maximum, minimum, ready, reported_pending, total, callback_types] : variants)
        {
            this->new_factory();
            this->make_pool(maximum);
            if (minimum != 0)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, minimum));
            }
            for (ULONG i = 0; i < ready; ++i)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            }

            const auto pending_address = this->storage + 0x800;
            emulator_object<LONG>{this->emu->memory, pending_address}.write(-1);
            SCOPED_TRACE(testing::Message() << "maximum " << maximum << " minimum " << minimum << " ready " << ready);
            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                {this->emu->memory, pending_address});
            }));
            const auto reported = emulator_object<LONG>{this->emu->memory, pending_address}.read();
            EXPECT_EQ(reported_pending, reported);

            ASSERT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage, 0));
            EXPECT_EQ(0u, field(0x2C));
            EXPECT_EQ(total, field(0x34));
            EXPECT_EQ(0u, field(0x24));
            EXPECT_EQ(0u, field(0x28));
            const auto shut_down = emulator_object<BOOLEAN>{this->emu->memory, this->storage + 0x1E}.read();
            EXPECT_EQ(1, shut_down);

            for (ULONG type = 0; type < callback_types.size(); ++type)
            {
                EXPECT_EQ(callback_types[type], this->set_callback_type(type)) << "type " << type;
            }

            EXPECT_EQ(abandoned, this->set_limit(WorkerFactoryThreadMinimum, 3));
            EXPECT_EQ(abandoned, this->set_limit(WorkerFactoryThreadMaximum, 3));
            for (const ULONG masks : {0u, 1u})
            {
                EXPECT_EQ(abandoned, this->set_information(WorkerFactoryThreadCpuSets, masks * 8, false));
            }
        }
    }

    // NtShutdownWorkerFactory adds the number of starting workers to the caller's count (ntoskrnl 10.0.26100.9974); the captured
    // rows are the ones with the count starting at -1.
    TEST_F(WorkerFactoryTest, ShutdownAddsTheStartingWorkersToTheCallersCount)
    {
        for (const LONG initial : {0, -1, 5})
        {
            for (const ULONG starting : {0u, 1u, 2u})
            {
                this->new_factory();
                this->make_pool(8);
                if (starting != 0)
                {
                    ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, starting));
                }

                const auto pending_address = this->storage + 0x800;
                emulator_object<LONG>{this->emu->memory, pending_address}.write(initial);
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                    {this->emu->memory, pending_address});
                }));
                const auto reported = emulator_object<LONG>{this->emu->memory, pending_address}.read();
                EXPECT_EQ(initial + static_cast<LONG>(starting), reported) << "initial " << initial << " starting " << starting;
                EXPECT_EQ(0u, this->pool().pending_worker_count);
                EXPECT_TRUE(this->pool().worker_threads.empty());
            }
        }
    }

    // Windows keeps its own counts: a worker that exits before it is ready stays in the pending count, and the other starting
    // worker, once ready, is counted whatever the pending count is. Shutdown drops only the workers that are still starting.
    TEST_F(WorkerFactoryTest, ReadyWorkerIsCountedWhileAnotherStartingWorkerLeft)
    {
        this->make_pool(8);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        const auto first_handle = this->pool().worker_threads[0];
        const auto second_handle = this->pool().worker_threads[1];
        this->emu->process.threads.get(first_handle)->exit_status = STATUS_SUCCESS;

        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(second_handle);
        ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());

        ASSERT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage, 0));
        const auto pending = emulator_object<ULONG>{this->emu->memory, this->storage + 0x2C}.read();
        const auto total = emulator_object<ULONG>{this->emu->memory, this->storage + 0x34}.read();
        EXPECT_EQ(1u, pending);
        EXPECT_EQ(1u, total);
        EXPECT_EQ(STATUS_SUCCESS, this->set_callback_type(2));

        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                            {this->emu->memory, this->storage + 0x7F0});
        }));
        EXPECT_EQ(std::vector<handle>{second_handle}, this->pool().worker_threads);
    }

    // Windows 11 26300: the caller's count is checked for alignment before anything is done, and reached after the factory is shut down
    // and its waiting workers are woken: a pointer that cannot be written (null included) fails the call and leaves the starting
    // workers counted. The add wraps at LONG_MAX.
    TEST_F(WorkerFactoryTest, ShutdownKeepsTheStartingWorkersWhenTheCountCannotBeReached)
    {
        enum class target
        {
            good,
            unmapped,
            null,
            read_only,
            misaligned
        };

        struct row
        {
            const char* name;
            target where;
            NTSTATUS status;
            bool shut_down;
        };

        for (const auto& [name, where, status, shut_down] :
             {row{"good", target::good, STATUS_SUCCESS, true}, row{"unmapped", target::unmapped, STATUS_ACCESS_VIOLATION, true},
              row{"null", target::null, STATUS_ACCESS_VIOLATION, true}, row{"read only", target::read_only, STATUS_ACCESS_VIOLATION, true},
              row{"misaligned", target::misaligned, STATUS_DATATYPE_MISALIGNMENT, false}})
        {
            this->new_factory();
            this->make_pool(8);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
            const auto valid = this->storage + 0x800;
            emulator_object<LONG>{this->emu->memory, valid}.write(7);
            if (where == target::read_only)
            {
                ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read, nullptr));
            }

            const uint64_t address = where == target::unmapped     ? 0x12345678
                                     : where == target::null       ? 0
                                     : where == target::misaligned ? valid + 1
                                                                   : valid;
            SCOPED_TRACE(name);
            EXPECT_EQ(status, this->emu->synchronize([&] {
                return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle, {this->emu->memory, address});
            }));

            if (where == target::read_only)
            {
                ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read_write, nullptr));
            }

            const bool succeeded = status == STATUS_SUCCESS;
            EXPECT_EQ(shut_down, this->pool().shutdown);
            EXPECT_EQ(shut_down ? 0u : 2u, this->pool().thread_minimum);
            EXPECT_EQ(shut_down ? 0u : 8u, this->pool().thread_maximum);
            EXPECT_EQ(succeeded ? 0u : 2u, this->pool().pending_worker_count);
            EXPECT_EQ(succeeded ? 0u : 2u, this->pool().worker_threads.size());
            if (succeeded)
            {
                const auto reported = emulator_object<LONG>{this->emu->memory, valid}.read();
                EXPECT_EQ(9, reported);
            }
        }

        this->new_factory();
        this->make_pool(8);
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
        emulator_object<LONG>{this->emu->memory, this->storage + 0x800}.write(std::numeric_limits<LONG>::max());
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                            {this->emu->memory, this->storage + 0x800});
        }));
        const auto wrapped = emulator_object<LONG>{this->emu->memory, this->storage + 0x800}.read();
        EXPECT_EQ(std::numeric_limits<LONG>::min(), wrapped);
    }

    // Windows 11 26300: every worker that waits when the factory is shut down wakes with status 0 and one empty packet, and every
    // wait after that is STATUS_ABANDONED with the packet untouched. Variants: one or two idle workers, and a worker that took a
    // normal packet first.
    TEST_F(WorkerFactoryTest, ShutdownWakesTheWaitingWorkersWithAnEmptyPacket)
    {
        constexpr uint64_t packet_address = 0x100;
        constexpr uint64_t returned_address = 0x80;
        constexpr uint64_t untouched = 0xAAAAAAAAAAAAAAAA;

        struct variant
        {
            ULONG workers;
            bool control_packet;
        };

        for (const auto& [workers, control_packet] : {variant{1, true}, variant{1, false}, variant{2, false}})
        {
            this->new_factory();
            this->make_pool(8);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, workers));
            std::vector<emulator_thread*> threads;
            for (ULONG i = 0; i < workers; ++i)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
                threads.push_back(this->emu->process.threads.get(this->pool().worker_threads[i]));
            }

            const auto packet_field = [&](const uint64_t index) {
                return emulator_object<uint64_t>{this->emu->memory, this->storage + packet_address + index * sizeof(uint64_t)}.read();
            };
            const auto fill_packet = [&] {
                for (uint64_t index = 0; index < 4; ++index)
                {
                    emulator_object<uint64_t>{this->emu->memory, this->storage + packet_address + index * sizeof(uint64_t)}.write(
                        untouched);
                }
            };
            const auto wait_abandoned = [&](emulator_thread& worker) {
                fill_packet();
                this->emu->vcpu(0).active_thread = &worker;
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context(), this->factory_handle,
                                                                          {this->emu->memory, this->storage + packet_address}, 1,
                                                                          {this->emu->memory, this->storage + returned_address}, 0);
                });
            };

            SCOPED_TRACE(testing::Message() << workers << " workers, control packet " << control_packet);
            if (control_packet)
            {
                fill_packet();
                this->wait_for_work(*threads.front());
                this->emu->synchronize([&] {
                    io_completion_message message{};
                    message.key_context = 0x11;
                    message.apc_context = 0x22;
                    message.io_status_block.Information = 0x33;
                    io_completion_wait::insert(*this->emu, this->port, *this->emu->process.io_completions.get(this->port), message);
                });
                EXPECT_EQ(std::optional<NTSTATUS>{STATUS_SUCCESS}, threads.front()->pending_status);
                EXPECT_EQ(0x11u, packet_field(0));
                EXPECT_EQ(0x22u, packet_field(1));
                EXPECT_EQ(0x33u, packet_field(3));
            }

            for (auto* worker : threads)
            {
                fill_packet();
                this->wait_for_work(*worker);
            }

            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                {this->emu->memory, this->storage + 0x7F0});
            }));

            for (auto* worker : threads)
            {
                EXPECT_FALSE(worker->await_io_completion.has_value());
                EXPECT_EQ(std::optional<NTSTATUS>{STATUS_SUCCESS}, worker->pending_status);
            }
            EXPECT_EQ(0u, packet_field(0));
            EXPECT_EQ(0u, packet_field(1));
            EXPECT_EQ(0u, packet_field(2));
            EXPECT_EQ(0u, packet_field(3));
            const auto returned = emulator_object<ULONG>{this->emu->memory, this->storage + returned_address}.read();
            EXPECT_EQ(1u, returned);

            ASSERT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage + 0x400, 0));
            const auto waiting = emulator_object<ULONG>{this->emu->memory, this->storage + 0x400 + 0x30}.read();
            const auto total = emulator_object<ULONG>{this->emu->memory, this->storage + 0x400 + 0x34}.read();
            EXPECT_EQ(0u, waiting);
            EXPECT_EQ(workers, total);

            for (auto* worker : threads)
            {
                EXPECT_EQ(STATUS_ABANDONED, wait_abandoned(*worker));
                EXPECT_EQ(untouched, packet_field(0));
            }
        }
    }

    // The packet a shut down factory posts passes itself on (ExpWorkerFactoryCompletionPacketRoutine, ntoskrnl 10.0.26100.9974): a thread
    // that is not a worker and waits on the same port can take it, and the workers still wake; the worker that took it is still
    // counted as waiting, so the thread that waits for the packet wakes too. Both orders of waiting.
    TEST_F(WorkerFactoryTest, ShutdownWakesTheWorkerWhenAnotherWaiterOnThePortTakesThePacket)
    {
        constexpr uint64_t untouched = 0xAAAAAAAAAAAAAAAA;

        for (const bool worker_waits_first : {true, false})
        {
            this->new_factory();
            this->make_pool(8);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
            auto* outsider = this->new_outsider();
            ASSERT_NE(nullptr, worker);
            ASSERT_NE(nullptr, outsider);

            const auto outsider_entry = this->storage + 0x200;
            const auto outsider_count = this->storage + 0x300;
            for (uint64_t index = 0; index < 4; ++index)
            {
                emulator_object<uint64_t>{this->emu->memory, outsider_entry + index * sizeof(uint64_t)}.write(untouched);
            }

            const auto outsider_waits = [&] {
                this->emu->vcpu(0).active_thread = outsider;
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtRemoveIoCompletionEx(this->context(), this->port, {this->emu->memory, outsider_entry}, 1,
                                                                   {this->emu->memory, outsider_count}, {this->emu->memory, 0}, FALSE);
                }));
                ASSERT_TRUE(outsider->await_io_completion.has_value());
            };

            SCOPED_TRACE(worker_waits_first ? "worker waits first" : "outsider waits first");
            if (worker_waits_first)
            {
                this->wait_for_work(*worker);
                outsider_waits();
            }
            else
            {
                outsider_waits();
                this->wait_for_work(*worker);
            }

            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                {this->emu->memory, this->storage + 0x7F0});
            }));

            EXPECT_FALSE(worker->await_io_completion.has_value());
            EXPECT_EQ(std::optional<NTSTATUS>{STATUS_SUCCESS}, worker->pending_status);
            EXPECT_FALSE(outsider->await_io_completion.has_value());
            const auto key = emulator_object<uint64_t>{this->emu->memory, outsider_entry}.read();
            const auto information = emulator_object<uint64_t>{this->emu->memory, outsider_entry + 3 * sizeof(uint64_t)}.read();
            const auto removed = emulator_object<ULONG>{this->emu->memory, outsider_count}.read();
            EXPECT_EQ(0u, key);
            EXPECT_EQ(0u, information);
            EXPECT_EQ(1u, removed);
        }
    }

    // The kernel's waiting count is raised by every NtWaitForWorkViaWorkerFactory wait on the factory, whoever calls it, and not by a plain
    // removal from the same port (ntoskrnl 10.0.26100.9974): a thread that is not a worker wakes at the shutdown, a worker that waits with
    // NtRemoveIoCompletionEx is not counted and gets nothing.
    TEST_F(WorkerFactoryTest, ShutdownCountsTheWaitsOnTheFactoryNotTheWorkersOnItsPort)
    {
        for (const bool worker_waits_for_work : {false, true})
        {
            this->new_factory();
            this->make_pool(8);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
            auto* outsider = this->new_outsider();
            ASSERT_NE(nullptr, worker);
            ASSERT_NE(nullptr, outsider);

            SCOPED_TRACE(worker_waits_for_work ? "a worker's plain removal" : "an outsider's wait for work");
            auto* waiter = worker_waits_for_work ? worker : outsider;
            if (worker_waits_for_work)
            {
                this->emu->vcpu(0).active_thread = waiter;
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtRemoveIoCompletionEx(this->context(), this->port, {this->emu->memory, this->storage + 0x100},
                                                                   1, {this->emu->memory, this->storage + 0x80}, {this->emu->memory, 0},
                                                                   FALSE);
                }));
            }
            else
            {
                this->wait_for_work(*waiter);
            }
            ASSERT_TRUE(waiter->await_io_completion.has_value());

            ASSERT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, 120, this->storage + 0x400, 0));
            const auto waiting = emulator_object<ULONG>{this->emu->memory, this->storage + 0x400 + 0x30}.read();
            EXPECT_EQ(worker_waits_for_work ? 0u : 1u, waiting);

            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                {this->emu->memory, this->storage + 0x7F0});
            }));
            EXPECT_EQ(worker_waits_for_work, waiter->await_io_completion.has_value());
            // The woken thread was still counted when the packet was removed, so the packet is posted once more and waits in the queue.
            EXPECT_EQ(worker_waits_for_work ? 0u : 1u, this->emu->process.io_completions.get(this->port)->queue.size());
        }
    }

    // A worker whose wait is suspended is skipped by the packet, which stays queued (ntoskrnl 10.0.26100.9974): a batch removal takes the
    // batch first and what the packet posts again waits for the next call, and a second shutdown posts nothing while the packet is out.
    TEST_F(WorkerFactoryTest, ShutdownWakeForASuspendedWorkerIsOnePacketTakenOnePerCall)
    {
        for (const ULONG shutdowns : {1u, 2u})
        {
            this->new_factory();
            this->make_pool(8);
            ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 1));
            ASSERT_EQ(STATUS_SUCCESS, this->worker_ready());
            auto* worker = this->emu->process.threads.get(this->pool().worker_threads.front());
            auto* outsider = this->new_outsider();
            ASSERT_NE(nullptr, worker);
            ASSERT_NE(nullptr, outsider);

            this->wait_for_work(*worker);
            worker->suspended = 1;
            SCOPED_TRACE(testing::Message() << shutdowns << " shutdowns");
            for (ULONG i = 0; i < shutdowns; ++i)
            {
                ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                    return syscalls::handle_NtShutdownWorkerFactory(this->context(), this->factory_handle,
                                                                    {this->emu->memory, this->storage + 0x7F0});
                }));
            }
            ASSERT_EQ(1u, this->emu->process.io_completions.get(this->port)->queue.size());

            this->emu->vcpu(0).active_thread = outsider;
            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtRemoveIoCompletionEx(this->context(), this->port, {this->emu->memory, this->storage + 0x200}, 2,
                                                               {this->emu->memory, this->storage + 0x300}, {this->emu->memory, 0}, FALSE);
            }));
            const auto removed = emulator_object<ULONG>{this->emu->memory, this->storage + 0x300}.read();
            EXPECT_EQ(1u, removed);
            EXPECT_EQ(1u, this->emu->process.io_completions.get(this->port)->queue.size());

            worker->suspended = 0;
            EXPECT_TRUE(this->is_ready(*worker));
            EXPECT_EQ(std::optional<NTSTATUS>{STATUS_SUCCESS}, worker->pending_status);
        }
    }

    // Windows 11 26300 (x64): the basic information is 120 bytes, the other classes are invalid.
    TEST_F(WorkerFactoryTest, QueryInformationAnswersOnlyTheBasicInformationLikeWindows)
    {
        constexpr ULONG basic_size = 120;
        constexpr ULONG sentinel = 0xFFFFFFFF;
        const uint64_t return_length = this->storage + 0x800;
        const auto returned = [&] { return emulator_object<ULONG>{this->emu->memory, return_length}.read(); };
        const auto reset_returned = [&] { emulator_object<ULONG>{this->emu->memory, return_length}.write(sentinel); };

        for (const ULONG info_class :
             std::views::iota(0u, 21u) | std::views::filter([](const ULONG c) { return c != WorkerFactoryBasicInformation; }))
        {
            for (const ULONG length : {256u, 8u, 0u})
            {
                reset_returned();
                EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_information(info_class, length, this->storage, return_length));
                EXPECT_EQ(sentinel, returned());
            }
        }
        EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_information(0, 0, 0, 0));

        for (const ULONG length : std::initializer_list<ULONG>{256, 8, 0, basic_size - 1, basic_size + 1})
        {
            reset_returned();
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH,
                      this->query_information(WorkerFactoryBasicInformation, length, this->storage, return_length));
            EXPECT_EQ(basic_size, returned());
        }

        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_information(WorkerFactoryBasicInformation, basic_size, 0, 0));

        reset_returned();
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_information(WorkerFactoryBasicInformation, 8, 0, return_length));
        EXPECT_EQ(sentinel, returned());

        for (const uint64_t offset : {2, 3, 5, 7})
        {
            EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                      this->query_information(WorkerFactoryBasicInformation, basic_size, this->storage + offset, return_length));
        }
        for (const uint64_t offset : {4, 8})
        {
            EXPECT_EQ(STATUS_SUCCESS,
                      this->query_information(WorkerFactoryBasicInformation, basic_size, this->storage + offset, return_length));
        }

        for (const ULONG length : std::initializer_list<ULONG>{0, 8, basic_size, 256})
        {
            reset_returned();
            EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT,
                      this->query_information(WorkerFactoryBasicInformation, length, this->storage + 1, return_length));
            EXPECT_EQ(sentinel, returned());
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_information(WorkerFactoryBasicInformation, length, this->storage, 1));
        }

        EXPECT_EQ(STATUS_INVALID_INFO_CLASS, this->query_information(0, basic_size, this->storage, return_length, true));
        EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH,
                  this->query_information(WorkerFactoryBasicInformation, 8, this->storage, return_length, true));
        EXPECT_EQ(STATUS_INVALID_HANDLE,
                  this->query_information(WorkerFactoryBasicInformation, basic_size, this->storage, return_length, true));
        EXPECT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, basic_size, this->storage, 0));

        ASSERT_EQ(STATUS_SUCCESS, this->set_idle_timeout(-20'000'000));
        ASSERT_EQ(STATUS_SUCCESS, this->set_information(WorkerFactoryBindingCount, sizeof(ULONG), false));
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMinimum, 2));
        ASSERT_EQ(STATUS_SUCCESS, this->set_limit(WorkerFactoryThreadMaximum, 5));
        reset_returned();
        ASSERT_EQ(STATUS_SUCCESS, this->query_information(WorkerFactoryBasicInformation, basic_size, this->storage, return_length));
        EXPECT_EQ(basic_size, returned());

        const auto i64_at = [&](const uint64_t offset) {
            return emulator_object<int64_t>{this->emu->memory, this->storage + offset}.read();
        };
        const auto u32_at = [&](const uint64_t offset) { return emulator_object<ULONG>{this->emu->memory, this->storage + offset}.read(); };
        EXPECT_EQ(-20'000'000, i64_at(0x10));
        EXPECT_EQ(3u, u32_at(0x20));
        EXPECT_EQ(2u, u32_at(0x24));
        EXPECT_EQ(5u, u32_at(0x28));
    }
}
