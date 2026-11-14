#include "emulation_test_utils.hpp"
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>
#include <worker_factory_support.hpp>

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
                factory.io_completion_handle = this->port;
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

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            handle factory_handle{};
            handle port{};
        };
    }

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
        worker->await_io_completion =
            pending_io_completion_wait{.io_completion_handle = this->port, .type = io_completion_wait_type::remove_multiple};

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
        ASSERT_TRUE(
            this->emu->synchronize([&] { return worker_factory_support::enqueue_release_completion(*this->emu, this->factory_handle); }));

        EXPECT_FALSE(worker->await_io_completion.has_value());
        EXPECT_EQ(1u, this->emu->process.io_completions.get(this->port)->queue.size());
        EXPECT_EQ(2u, factory.worker_threads.size());
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
