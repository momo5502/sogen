#include "std_include.hpp"

#include <io_completion_wait.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtAlertThread(const syscall_context& c, handle thread_handle);
    NTSTATUS handle_NtQueueApcThread(const syscall_context& c, handle thread_handle, uint64_t apc_routine, uint64_t apc_argument1,
                                     uint64_t apc_argument2, uint64_t apc_argument3);
    NTSTATUS handle_NtTestAlert(const syscall_context& c);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, handle h, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtDelayExecution(const syscall_context& c, BOOLEAN alertable, emulator_object<LARGE_INTEGER> delay_interval);
    NTSTATUS handle_NtContinueEx(const syscall_context& c, uint64_t thread_context, uint64_t continue_argument);
    NTSTATUS handle_NtAssociateWaitCompletionPacket(const syscall_context& c, handle wait_completion_packet_handle,
                                                    handle io_completion_handle, handle target_object_handle, emulator_pointer key_context,
                                                    emulator_pointer apc_context, NTSTATUS io_status,
                                                    EmulatorTraits<Emu64>::ULONG_PTR io_status_information,
                                                    emulator_object<BOOLEAN> already_signaled);
    NTSTATUS handle_NtCancelWaitCompletionPacket(const syscall_context& c, handle wait_completion_packet_handle,
                                                 BOOLEAN remove_signaled_packet);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtRemoveIoCompletion(const syscall_context& c, handle io_completion_handle,
                                         emulator_object<emulator_pointer> key_context, emulator_object<emulator_pointer> apc_context,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                         emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtRemoveIoCompletionEx(const syscall_context& c, handle io_completion_handle,
                                           emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information,
                                           ULONG count, emulator_object<ULONG> num_entries_removed, emulator_object<LARGE_INTEGER> timeout,
                                           BOOLEAN alertable);
    NTSTATUS handle_NtSetIoCompletion(const syscall_context& c, handle io_completion_handle, emulator_pointer key_context,
                                      emulator_pointer apc_context, NTSTATUS io_status,
                                      EmulatorTraits<Emu64>::ULONG_PTR io_status_information);
    NTSTATUS handle_NtReleaseWorkerFactoryWorker(const syscall_context& c, handle worker_factory_handle);
    NTSTATUS handle_NtWaitForWorkViaWorkerFactory(const syscall_context& c, handle worker_factory_handle,
                                                  emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> mini_packets,
                                                  ULONG count, emulator_object<ULONG> packets_returned, emulator_pointer deferred_work);
}

namespace sogen::test
{
    namespace
    {
        class wait_process_manager final : public process_manager
        {
          public:
            process_exit_status_result result{};

            process_create_result create_process(process_create_request) override
            {
                return {.error = process_error::unavailable};
            }

            process_error resume_process(managed_process) override
            {
                return process_error::unavailable;
            }

            process_error terminate_process(managed_process, uint64_t) override
            {
                return process_error::unavailable;
            }

            process_exit_status_result exit_status(managed_process) const override
            {
                return this->result;
            }

            process_memory_result allocate_memory(managed_process, uint64_t, uint64_t, memory_permission, bool, bool) override
            {
                return {.error = process_error::unavailable};
            }

            process_error free_memory(managed_process, uint64_t, uint64_t, bool) override
            {
                return process_error::unavailable;
            }

            process_memory_result protect_memory(managed_process, uint64_t, uint64_t, memory_permission) override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_read_result read_memory(managed_process, uint64_t, uint64_t) const override
            {
                return {.error = process_error::unavailable};
            }

            process_memory_result write_memory(managed_process, uint64_t, std::span<const uint8_t>) override
            {
                return {.error = process_error::unavailable};
            }
        };

        class ManagedWaitCompletionTest : public testing::TestWithParam<bool>
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.processes = &this->manager;
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                ASSERT_NE(this->emu->process.processes.get(GUEST_PROCESS_HANDLE), nullptr);
                if (GetParam())
                {
                    managed_process_thread thread{};
                    thread.process.value = 1;
                    this->target = this->emu->process.managed_threads.store(std::move(thread));
                }
                else
                {
                    emulator_process process{};
                    process.process.value = 1;
                    this->target = this->emu->process.processes.store(std::move(process));
                }
                this->completion = this->emu->process.io_completions.store(io_completion{});
                this->packet = this->emu->process.wait_completion_packets.store(wait_completion_packet{});
                this->signaled =
                    emulator_object<BOOLEAN>{this->emu->memory, this->emu->memory.allocate_memory(0x1000, memory_permission::read_write)};
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS associate()
            {
                return syscalls::handle_NtAssociateWaitCompletionPacket(this->context(), this->packet, this->completion, this->target,
                                                                        0x1234, 0x5678, STATUS_SUCCESS, 42, *this->signaled);
            }

            wait_process_manager manager{};
            std::unique_ptr<windows_emulator> emu{};
            handle target{};
            handle completion{};
            handle packet{};
            std::optional<emulator_object<BOOLEAN>> signaled{};
        };
    }

    TEST_P(ManagedWaitCompletionTest, PendingWaitRetainsTargetAndCompletesOnceOnExit)
    {
        ASSERT_EQ(this->associate(), STATUS_SUCCESS);
        EXPECT_FALSE(this->signaled->read());
        EXPECT_EQ(syscalls::handle_NtClose(this->context(), this->target), STATUS_SUCCESS);

        io_completion_message message{};
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
        this->manager.result.error = process_error::communication_failure;
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));

        this->manager.result = {.status = process_exit{.code = 7}};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
        EXPECT_EQ(message.key_context, 0x1234u);
        EXPECT_EQ(message.apc_context, 0x5678u);
        EXPECT_EQ(message.io_status_block.Status, STATUS_SUCCESS);
        EXPECT_EQ(message.io_status_block.Information, 42u);
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
        if (GetParam())
        {
            EXPECT_EQ(this->emu->process.managed_threads.get(this->target), nullptr);
        }
        else
        {
            EXPECT_EQ(this->emu->process.processes.get(this->target), nullptr);
        }
    }

    TEST_P(ManagedWaitCompletionTest, ExitedTargetSignalsImmediatelyAndCanBeRearmed)
    {
        this->manager.result = {.status = process_exit{.code = 0}};
        ASSERT_EQ(this->associate(), STATUS_SUCCESS);
        EXPECT_TRUE(this->signaled->read());
        io_completion_message message{};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
        ASSERT_EQ(this->associate(), STATUS_SUCCESS);
        EXPECT_TRUE(this->signaled->read());
        EXPECT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
    }

    TEST_P(ManagedWaitCompletionTest, ClosedTargetIsRejected)
    {
        ASSERT_EQ(syscalls::handle_NtClose(this->context(), this->target), STATUS_SUCCESS);
        EXPECT_EQ(this->associate(), STATUS_INVALID_HANDLE);
    }

    INSTANTIATE_TEST_SUITE_P(ProcessAndThread, ManagedWaitCompletionTest, testing::Bool());

    namespace
    {
        class stdin_console_backend final : public console_backend
        {
          public:
            bool available{false};

            bool input_available() override
            {
                return this->available;
            }

            std::optional<console_key_event> read_input_event(bool /*wait*/, bool /*remove*/) override
            {
                return std::nullopt;
            }

            std::string read_input(size_t /*length*/) override
            {
                return {};
            }

            std::optional<std::string> try_read_input(size_t /*length*/) override
            {
                return std::string{};
            }

            void set_input_mode(const console_input_mode& /*mode*/) override
            {
            }

            void reset() override
            {
            }
        };

        class ConsoleWaitCompletionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                auto backend = std::make_unique<stdin_console_backend>();
                this->console = backend.get();

                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::move(backend);
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->completion = this->emu->process.io_completions.store(io_completion{});
                this->packet = this->emu->process.wait_completion_packets.store(wait_completion_packet{});
                this->signaled =
                    emulator_object<BOOLEAN>{this->emu->memory, this->emu->memory.allocate_memory(0x1000, memory_permission::read_write)};
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS associate(const handle target)
            {
                return syscalls::handle_NtAssociateWaitCompletionPacket(this->context(), this->packet, this->completion, target, 0x1234,
                                                                        0x5678, STATUS_SUCCESS, 42, *this->signaled);
            }

            stdin_console_backend* console{};
            std::unique_ptr<windows_emulator> emu{};
            handle completion{};
            handle packet{};
            std::optional<emulator_object<BOOLEAN>> signaled{};
        };
    }

    // libuv registers a thread-pool wait on the console input handle to read a raw TTY, so a guest
    // cannot start a terminal UI unless a wait packet can target stdin.
    TEST_F(ConsoleWaitCompletionTest, StdinPacketCompletesOnlyOnceInputArrives)
    {
        ASSERT_EQ(this->associate(STDIN_HANDLE), STATUS_SUCCESS);
        EXPECT_FALSE(this->signaled->read());

        io_completion_message message{};
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));

        this->console->available = true;
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
        EXPECT_EQ(message.key_context, 0x1234u);
        EXPECT_EQ(message.apc_context, 0x5678u);
    }

    TEST_F(ConsoleWaitCompletionTest, ConsoleOutputHandleStaysUnsupported)
    {
        EXPECT_EQ(this->associate(STDOUT_HANDLE), STATUS_OBJECT_TYPE_MISMATCH);
    }

    TEST_F(ConsoleWaitCompletionTest, RealFileHandleStaysUnsupported)
    {
        const auto file_handle = this->emu->process.files.store(file{});
        EXPECT_EQ(this->associate(file_handle), STATUS_OBJECT_TYPE_MISMATCH);
    }

    using WaitCompletionCancelTest = ConsoleWaitCompletionTest;

    // Expected values captured on Windows 11 (build 26200), x64 and WoW64.
    TEST_F(WaitCompletionCancelTest, StatusAndPacketFollowTheWaitState)
    {
        struct cancel_case
        {
            const char* row;
            bool associate;
            bool signal_before_cancel;
            bool dequeue_before_cancel;
            BOOLEAN remove_signaled_packet;
            NTSTATUS status;
            std::optional<NTSTATUS> second_status;
            ULONG packets_after;
        };

        const std::array<cancel_case, 6> cases{{
            {"never associated", false, false, false, FALSE, STATUS_CANCELLED, STATUS_CANCELLED, 0},
            {"unsignalled, keep", true, false, false, FALSE, STATUS_SUCCESS, STATUS_CANCELLED, 0},
            {"unsignalled, remove", true, false, false, TRUE, STATUS_SUCCESS, std::nullopt, 0},
            {"queued, keep", true, true, false, FALSE, STATUS_PENDING, STATUS_PENDING, 1},
            {"queued, remove", true, true, false, TRUE, STATUS_SUCCESS, STATUS_CANCELLED, 0},
            {"already dequeued", true, true, true, FALSE, STATUS_CANCELLED, std::nullopt, 0},
        }};

        for (const auto& c : cases)
        {
            SCOPED_TRACE(c.row);
            const auto wait_packet = this->emu->process.wait_completion_packets.store(wait_completion_packet{});
            event synchronization_event{};
            synchronization_event.type = SynchronizationEvent;
            const auto target = this->emu->process.events.store(std::move(synchronization_event));
            auto& target_event = *this->emu->process.events.get(target);

            if (c.associate)
            {
                ASSERT_EQ(syscalls::handle_NtAssociateWaitCompletionPacket(this->context(), wait_packet, this->completion, target, 0x1234,
                                                                           0x5678, STATUS_SUCCESS, 42, *this->signaled),
                          STATUS_SUCCESS);
            }

            io_completion_message message{};
            target_event.set_signaled(c.signal_before_cancel);
            if (c.dequeue_before_cancel)
            {
                ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message));
            }

            EXPECT_EQ(syscalls::handle_NtCancelWaitCompletionPacket(this->context(), wait_packet, c.remove_signaled_packet), c.status);
            if (c.second_status)
            {
                EXPECT_EQ(syscalls::handle_NtCancelWaitCompletionPacket(this->context(), wait_packet, c.remove_signaled_packet),
                          *c.second_status);
            }

            if (!c.signal_before_cancel)
            {
                target_event.set_signaled(true);
            }

            ULONG packets = 0;
            while (io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message))
            {
                ++packets;
            }
            EXPECT_EQ(packets, c.packets_after);
            EXPECT_EQ(target_event.is_signaled(), !c.signal_before_cancel);

            EXPECT_EQ(syscalls::handle_NtClose(this->context(), wait_packet), STATUS_SUCCESS);
            EXPECT_EQ(syscalls::handle_NtClose(this->context(), target), STATUS_SUCCESS);
        }
    }

    namespace
    {
        // Threads that wait on one port in NtRemoveIoCompletion, each with its own slot for the packet it gets.
        class IoCompletionWaiterTest : public testing::Test
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
                this->port = this->emu->process.io_completions.store(io_completion{});
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            // Each with its own id: a port names its waiters by thread id.
            emulator_thread& new_thread()
            {
                this->next_thread_id += 4;
                const auto thread = this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, this->next_thread_id, true});
                return *this->emu->process.threads.get(thread);
            }

            syscall_context context_of(emulator_thread& thread)
            {
                this->emu->vcpu(0).active_thread = &thread;
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<emulator_pointer> key(const size_t slot) const
            {
                return {this->emu->memory, this->storage + slot * 0x40};
            }

            NTSTATUS remove(emulator_thread& thread, const size_t slot, const std::optional<int64_t> timeout = {})
            {
                emulator_object<LARGE_INTEGER> timeout_object{this->emu->memory};
                if (timeout)
                {
                    LARGE_INTEGER value{};
                    value.QuadPart = *timeout;
                    timeout_object.set_address(this->storage + 0x800 + slot * 0x10);
                    timeout_object.write(value);
                }
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtRemoveIoCompletion(this->context_of(thread), this->port, this->key(slot),
                                                                 {this->emu->memory, this->storage + slot * 0x40 + 8},
                                                                 {this->emu->memory, this->storage + slot * 0x40 + 0x10}, timeout_object);
                });
            }

            NTSTATUS post(emulator_thread& thread, const uint64_t key_context)
            {
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtSetIoCompletion(this->context_of(thread), this->port, key_context, 0, STATUS_SUCCESS, 0);
                });
            }

            io_completion& completion()
            {
                return *this->emu->process.io_completions.get(this->port);
            }

            std::unique_ptr<windows_emulator> emu{};
            handle port{};
            uint64_t storage{};
            uint32_t next_thread_id{0x100};
        };
    }

    // KeRemoveQueueEx puts a new waiter at the head of the queue's wait list, and KeInsertQueue (KiWakeQueueWaiter) gives
    // the packet to the head: the latest waiter is served first.
    TEST_F(IoCompletionWaiterTest, PacketGoesToTheLatestWaiter)
    {
        auto& first = this->new_thread();
        auto& second = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(first, 0));
        ASSERT_EQ(STATUS_SUCCESS, this->remove(second, 1));

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        EXPECT_TRUE(first.await_io_completion.has_value());
        EXPECT_FALSE(second.await_io_completion.has_value());
        EXPECT_EQ(std::optional<NTSTATUS>{STATUS_SUCCESS}, second.pending_status);
        EXPECT_EQ(0x11u, this->key(1).read());

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        EXPECT_FALSE(first.await_io_completion.has_value());
        EXPECT_EQ(0x22u, this->key(0).read());
        EXPECT_TRUE(this->completion().queue.empty());
    }

    // The packet is the waiter's wait status the moment it is queued, so a caller that does not wait finds the port empty.
    TEST_F(IoCompletionWaiterTest, PacketHandedToAWaiterIsNotLeftInThePort)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));

        EXPECT_EQ(STATUS_TIMEOUT, this->remove(poster, 1, 0));
        EXPECT_EQ(0x11u, this->key(0).read());
    }

    TEST_F(IoCompletionWaiterTest, PacketSkipsAWaiterWhoseWaitEnded)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        waiter.mark_as_ready(STATUS_USER_APC);

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        EXPECT_EQ(1u, this->completion().queue.size());
        EXPECT_TRUE(this->completion().waiters.empty());
    }

    // A suspended thread is out of the queue's wait list until it resumes; then KeRemoveQueueEx takes what is queued.
    TEST_F(IoCompletionWaiterTest, SuspendedWaiterTakesTheQueuedPacketOnceResumed)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        waiter.suspended = 1;

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        EXPECT_EQ(1u, this->completion().queue.size());
        EXPECT_TRUE(waiter.await_io_completion.has_value());

        waiter.suspended = 0;
        EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
        EXPECT_EQ(0x11u, this->key(0).read());
        EXPECT_TRUE(this->completion().queue.empty());
    }

    // NtWaitForWorkViaWorkerFactory keeps its count to itself until it succeeds (19041 on the host): a worker's wait writes
    // nothing to PacketsReturned when it starts, nor when it ends without work, as the factory's timeout ends it here.
    TEST_F(IoCompletionWaiterTest, WorkerWaitStoresItsCountOnlyWhenItTakesWork)
    {
        constexpr ULONG sentinel = 0xDEADBEEF;
        constexpr int64_t one_second = -10'000'000;
        auto& worker = this->new_thread();
        worker_factory factory{};
        factory.io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{});
        factory.timeout = one_second;
        const auto factory_handle = this->emu->process.worker_factories.store(std::move(factory));

        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> packets{this->emu->memory, this->storage};
        const emulator_object<ULONG> returned{this->emu->memory, this->storage + 0x100};
        returned.write(sentinel);
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context_of(worker), factory_handle, packets, 1, returned, 0);
        }));
        EXPECT_EQ(sentinel, returned.read());

        auto& clock = this->emu->scheduling_clock();
        clock.continue_time_at(clock.steady_now() + std::chrono::seconds(2), clock.timestamp_counter());
        EXPECT_TRUE(this->emu->synchronize([&] { return worker.is_thread_ready(*this->emu); }));
        EXPECT_EQ(STATUS_TIMEOUT, worker.pending_status);
        EXPECT_EQ(sentinel, returned.read());
    }

    // A worker whose factory timeout passed before it waits gets STATUS_TIMEOUT at once, and its count is not stored either.
    TEST_F(IoCompletionWaiterTest, WorkerWaitPastItsTimeoutStoresNoCount)
    {
        constexpr ULONG sentinel = 0xDEADBEEF;
        constexpr int64_t seconds_ago = 10;
        auto& worker = this->new_thread();
        worker_factory factory{};
        factory.io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{});
        factory.timeout =
            utils::convert_unix_to_windows_time(std::chrono::system_clock::to_time_t(this->emu->clock().system_now()) - seconds_ago)
                .QuadPart;
        const auto factory_handle = this->emu->process.worker_factories.store(std::move(factory));

        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> packets{this->emu->memory, this->storage};
        const emulator_object<ULONG> returned{this->emu->memory, this->storage + 0x100};
        returned.write(sentinel);
        EXPECT_EQ(STATUS_TIMEOUT, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context_of(worker), factory_handle, packets, 1, returned, 0);
        }));
        EXPECT_EQ(sentinel, returned.read());
    }

    // The resumed waiter takes the queued packet before it is back in the wait list, so the next worker factory release,
    // which taking the first one queues, stays queued for another wait instead of reaching the same one and being lost.
    TEST_F(IoCompletionWaiterTest, ResumedWorkerTakesOneReleaseAndLeavesTheNextQueued)
    {
        auto& worker = this->new_thread();
        auto& releaser = this->new_thread();
        worker_factory factory{};
        factory.io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{});
        const auto factory_handle = this->emu->process.worker_factories.store(std::move(factory));

        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> packets{this->emu->memory, this->storage};
        const emulator_object<ULONG> returned{this->emu->memory, this->storage + 0x100};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context_of(worker), factory_handle, packets, 1, returned, 0);
        }));
        worker.suspended = 1;
        for (int i = 0; i < 2; ++i)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtReleaseWorkerFactoryWorker(this->context_of(releaser), factory_handle);
            }));
        }

        worker.suspended = 0;
        EXPECT_TRUE(this->emu->synchronize([&] { return worker.is_thread_ready(*this->emu); }));
        EXPECT_EQ(STATUS_SUCCESS, worker.pending_status);
        EXPECT_EQ(1u, returned.read());
        EXPECT_EQ(1u, this->emu->process.worker_factories.get(factory_handle)->pending_release_count);
        EXPECT_EQ(1u, this->completion().queue.size());
    }

    // The resumed waiter's copy is the kernel's too: an output reprotected while it was suspended fails the wait with
    // STATUS_ACCESS_VIOLATION, and the packet, taken off the queue, is gone.
    TEST_F(IoCompletionWaiterTest, SuspendedWaiterWhoseOutputWasReprotectedFailsAndThePacketIsGone)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        waiter.suspended = 1;
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read, nullptr));

        waiter.suspended = 0;
        EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read_write, nullptr));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, waiter.pending_status);
        EXPECT_TRUE(this->completion().queue.empty());
        EXPECT_EQ(0u, this->key(0).read());
    }

    // The probes before the wait take a guard page's guard and fail the call with STATUS_GUARD_PAGE_VIOLATION (19041 on the
    // host).
    TEST_F(IoCompletionWaiterTest, OutputsOnAGuardPageFailTheRemoveWithItsGuard)
    {
        auto& waiter = this->new_thread();
        const auto guarded = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const auto guard = [&] {
            return this->emu->memory.protect_memory(guarded, 0x1000,
                                                    nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard));
        };
        const emulator_object<LARGE_INTEGER> no_wait{this->emu->memory, this->storage + 0x900};
        no_wait.write(LARGE_INTEGER{});

        ASSERT_TRUE(guard());
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletion(this->context_of(waiter), this->port, {this->emu->memory, guarded},
                                                         {this->emu->memory, this->storage + 8}, {this->emu->memory, this->storage + 0x10},
                                                         no_wait);
        }));
        ASSERT_TRUE(guard());
        EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletionEx(this->context_of(waiter), this->port, {this->emu->memory, guarded}, 1,
                                                           {this->emu->memory, this->storage + 0x20}, no_wait, FALSE);
        }));
        EXPECT_FALSE(this->emu->memory.is_guard_page(guarded));
    }

    // IoRemoveIoCompletion stores each taken entry under try, and NtRemoveIoCompletionEx stores the count only when none
    // faulted: a count reprotected while the waiter was suspended fails the wait after both entries are written.
    TEST_F(IoCompletionWaiterTest, SuspendedExWaiterWritesItsEntriesButNotACountReprotectedMeanwhile)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        const auto count_page = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> entries{this->emu->memory, this->storage};
        const emulator_object<ULONG> removed{this->emu->memory, count_page};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletionEx(this->context_of(waiter), this->port, entries, 2, removed, {this->emu->memory},
                                                           FALSE);
        }));
        waiter.suspended = 1;
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        ASSERT_TRUE(this->emu->memory.protect_memory(count_page, 0x1000, memory_permission::read, nullptr));

        waiter.suspended = 0;
        EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, waiter.pending_status);
        EXPECT_TRUE(this->completion().queue.empty());
        EXPECT_EQ(0x11u, entries.read(0).KeyContext);
        EXPECT_EQ(0x22u, entries.read(1).KeyContext);
        EXPECT_EQ(0u, removed.read());
    }

    // NtRemoveIoCompletionEx writes nothing when its wait starts, and a wait that times out stores the count of
    // KeRemoveQueueEx's one entry, STATUS_TIMEOUT: 1, with the entries as they were (19041 on the host).
    TEST_F(IoCompletionWaiterTest, ExWaitThatTimesOutStoresACountOfOne)
    {
        using entry_type = FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>;
        constexpr uint64_t sentinel_key = 0xAB;
        constexpr int64_t one_second = -10'000'000;
        auto& waiter = this->new_thread();
        const emulator_object<entry_type> entries{this->emu->memory, this->storage + 0x100};
        const emulator_object<ULONG> removed{this->emu->memory, this->storage + 0x20};
        const emulator_object<LARGE_INTEGER> timeout{this->emu->memory, this->storage + 0x900};
        entry_type sentinel{};
        sentinel.KeyContext = sentinel_key;
        entries.write(sentinel);
        removed.write(0xDEADBEEF);
        LARGE_INTEGER due{};
        due.QuadPart = one_second;
        timeout.write(due);

        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletionEx(this->context_of(waiter), this->port, entries, 2, removed, timeout, FALSE);
        }));
        EXPECT_EQ(0xDEADBEEFu, removed.read());

        auto& clock = this->emu->scheduling_clock();
        clock.continue_time_at(clock.steady_now() + std::chrono::seconds(2), clock.timestamp_counter());
        EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
        EXPECT_EQ(STATUS_TIMEOUT, waiter.pending_status);
        EXPECT_EQ(1u, removed.read());
        EXPECT_EQ(sentinel_key, entries.read().KeyContext);
    }

    // The copy-out after the wait is plain stores under try (NtRemoveIoCompletion): a store needs a writable page before its
    // guard is looked at, and the wait ends with its fault's own code. A KeyContext that became a guard page while the waiter
    // was suspended fails the wait with STATUS_GUARD_PAGE_VIOLATION and loses its guard; a read-only guard page fails it with
    // STATUS_ACCESS_VIOLATION and keeps its guard (19041 on the host).
    TEST_F(IoCompletionWaiterTest, CopyOutStoresEndTheWaitWithTheirFaultsCode)
    {
        for (const auto& [permission, expected, keeps_guard] :
             {std::tuple{memory_permission::read_write, STATUS_GUARD_PAGE_VIOLATION, false},
              std::tuple{memory_permission::read, STATUS_ACCESS_VIOLATION, true}})
        {
            auto& waiter = this->new_thread();
            auto& poster = this->new_thread();
            const auto key_page = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtRemoveIoCompletion(this->context_of(waiter), this->port, {this->emu->memory, key_page},
                                                             {this->emu->memory, this->storage + 8},
                                                             {this->emu->memory, this->storage + 0x10}, {this->emu->memory});
            }));
            waiter.suspended = 1;
            ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
            ASSERT_TRUE(this->emu->memory.protect_memory(key_page, 0x1000, nt_memory_permission(permission, memory_permission_ext::guard),
                                                         nullptr));

            waiter.suspended = 0;
            EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
            EXPECT_EQ(expected, waiter.pending_status);
            EXPECT_EQ(keeps_guard, this->emu->memory.is_guard_page(key_page));
        }
    }

    // IoRemoveIoCompletion stores each taken entry under try and goes on after a fault, returning the code it saved last: of
    // two entries on two pages the second's fault is the result (19041 on the host).
    TEST_F(IoCompletionWaiterTest, CopyOutOfAnEntryArrayEndsWithTheLastFaultsCode)
    {
        const auto guard = nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard);
        const auto read_only = nt_memory_permission(memory_permission::read);
        for (const auto& [first, second, expected] :
             {std::tuple{guard, read_only, STATUS_ACCESS_VIOLATION}, std::tuple{read_only, guard, STATUS_GUARD_PAGE_VIOLATION}})
        {
            using entry_type = FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>;
            auto& waiter = this->new_thread();
            auto& poster = this->new_thread();
            const auto pages = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
            const emulator_object<entry_type> entries{this->emu->memory, pages + 0x1000 - sizeof(entry_type)};
            const emulator_object<ULONG> removed{this->emu->memory, this->storage + 0x20};
            ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
                return syscalls::handle_NtRemoveIoCompletionEx(this->context_of(waiter), this->port, entries, 2, removed,
                                                               {this->emu->memory}, FALSE);
            }));
            waiter.suspended = 1;
            ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
            ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
            ASSERT_TRUE(this->emu->memory.protect_memory(pages, 0x1000, first, nullptr));
            ASSERT_TRUE(this->emu->memory.protect_memory(pages + 0x1000, 0x1000, second, nullptr));

            waiter.suspended = 0;
            EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
            EXPECT_EQ(expected, waiter.pending_status);
        }
    }

    // NtRemoveIoCompletion probes only the status block's first ULONG, then stores the taken packet under try: KeyContext,
    // ApcContext, then the whole block in one store. A block that runs into a read-only page fails the call after the
    // contexts are stored, and the packet is gone.
    TEST_F(IoCompletionWaiterTest, StatusBlockRunningIntoAReadOnlyPageFailsTheCallAndThePacketIsGone)
    {
        auto& thread = this->new_thread();
        const auto pages = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_TRUE(this->emu->memory.protect_memory(pages + 0x1000, 0x1000, memory_permission::read, nullptr));
        const emulator_object<uint64_t> status_block_start{this->emu->memory, pages + 0xFF8};
        status_block_start.write(~0ULL);

        ASSERT_EQ(STATUS_SUCCESS, this->post(thread, 0x11));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletion(this->context_of(thread), this->port, {this->emu->memory, pages},
                                                         {this->emu->memory, pages + 8}, {this->emu->memory, pages + 0xFF8},
                                                         {this->emu->memory});
        }));
        EXPECT_TRUE(this->completion().queue.empty());
        EXPECT_EQ(0x11u, emulator_object<emulator_pointer>(this->emu->memory, pages).read());
        EXPECT_EQ(~0ULL, status_block_start.read());
    }

    // A packet inserted after the resume but before the resumed waiter's readiness check queues behind the earlier one, as
    // KeInsertQueue does with packets queued, so the waiter still takes the first.
    TEST_F(IoCompletionWaiterTest, PacketInsertedBeforeAResumedWaiterRunsQueuesBehindTheEarlierOne)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        waiter.suspended = 1;
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));

        waiter.suspended = 0;
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        EXPECT_EQ(2u, this->completion().queue.size());

        EXPECT_TRUE(this->emu->synchronize([&] { return waiter.is_thread_ready(*this->emu); }));
        EXPECT_EQ(0x11u, this->key(0).read());
        ASSERT_EQ(1u, this->completion().queue.size());
        EXPECT_EQ(0x22u, this->completion().queue.front().key_context);
    }

    // The outputs are probed before the wait: a read-only one fails the call and leaves the packet queued. One reprotected
    // while its thread waits fails that wait when a packet arrives, and the packet, taken off the queue, is gone, as when
    // the kernel's copy after the wait faults.
    TEST_F(IoCompletionWaiterTest, OutputsAreProbedBeforeTheWaitAndCheckedWhenThePacketArrives)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        const auto read_only = this->emu->memory.allocate_memory(0x1000, memory_permission::read);

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->emu->synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletion(this->context_of(waiter), this->port, {this->emu->memory, read_only},
                                                         {this->emu->memory, this->storage + 8}, {this->emu->memory, this->storage + 0x10},
                                                         {this->emu->memory});
        }));
        EXPECT_EQ(1u, this->completion().queue.size());
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        ASSERT_TRUE(this->completion().queue.empty());

        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));
        ASSERT_TRUE(waiter.await_io_completion.has_value());
        ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read, nullptr));
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        ASSERT_TRUE(this->emu->memory.protect_memory(this->storage, 0x1000, memory_permission::read_write, nullptr));

        EXPECT_FALSE(waiter.await_io_completion.has_value());
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, waiter.pending_status);
        EXPECT_TRUE(this->completion().queue.empty());
        EXPECT_NE(0x22u, this->key(0).read());
    }

    // Nothing polls the port for a waiting thread: a scheduler pass queues a signaled wait packet's completion, and the
    // insert hands it to the waiter.
    TEST_F(IoCompletionWaiterTest, SignaledWaitPacketReachesTheWaiterInTheNextPass)
    {
        auto& waiter = this->new_thread();
        const auto target = this->emu->process.events.store(event{});
        const auto packet = this->emu->process.wait_completion_packets.store(wait_completion_packet{});
        const emulator_object<BOOLEAN> already_signaled{this->emu->memory, this->storage + 0xF00};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtAssociateWaitCompletionPacket(this->context_of(waiter), packet, this->port, target,
                                                                                   0x33, 0x44, STATUS_SUCCESS, 0, already_signaled));
        ASSERT_FALSE(already_signaled.read());
        ASSERT_EQ(STATUS_SUCCESS, this->remove(waiter, 0));

        this->emu->process.events.get(target)->set_signaled(true);
        EXPECT_TRUE(waiter.await_io_completion.has_value());

        EXPECT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_FALSE(waiter.await_io_completion.has_value());
        EXPECT_EQ(0x33u, this->key(0).read());
    }

    namespace
    {
        // One wait packet on the fixture's port that moves between two notification events (they stay signaled once set,
        // so a packet still checking its old target would fire as soon as it is associated again).
        class WaitPacketTargetTest : public IoCompletionWaiterTest
        {
          protected:
            void SetUp() override
            {
                IoCompletionWaiterTest::SetUp();
                this->first = this->emu->process.events.store(event{});
                this->second = this->emu->process.events.store(event{});
                this->packet = this->emu->process.wait_completion_packets.store(wait_completion_packet{});
                this->already_signaled = emulator_object<BOOLEAN>{this->emu->memory, this->storage + 0xF00};
                this->caller = &this->new_thread();
            }

            NTSTATUS associate(const handle target)
            {
                return syscalls::handle_NtAssociateWaitCompletionPacket(this->context_of(*this->caller), this->packet, this->port, target,
                                                                        0x33, 0x44, STATUS_SUCCESS, 0, *this->already_signaled);
            }

            void set(const handle target)
            {
                this->emu->process.events.get(target)->set_signaled(true);
            }

            size_t queued()
            {
                io_completion_wait::materialize_all_signaled_wait_packets(*this->emu);
                return this->completion().queue.size();
            }

            handle first{};
            handle second{};
            handle packet{};
            std::optional<emulator_object<BOOLEAN>> already_signaled{};
            emulator_thread* caller{};
        };
    }

    TEST_F(WaitPacketTargetTest, PacketAssociatedAgainAfterItsCompletionWatchesTheNewTarget)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->first));
        this->set(this->first);
        ASSERT_EQ(1u, this->queued());
        io_completion_message message{};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, this->port, message));

        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->second));
        EXPECT_FALSE(this->already_signaled->read());
        EXPECT_EQ(0u, this->queued());
        this->set(this->second);
        EXPECT_EQ(1u, this->queued());
    }

    TEST_F(WaitPacketTargetTest, PacketAssociatedAgainAfterACancelWatchesTheNewTarget)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->first));
        ASSERT_EQ(0u, this->queued());
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelWaitCompletionPacket(this->context_of(*this->caller), this->packet, FALSE));
        this->set(this->first);

        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->second));
        EXPECT_FALSE(this->already_signaled->read());
        EXPECT_EQ(0u, this->queued());
        this->set(this->second);
        EXPECT_EQ(1u, this->queued());
    }

    TEST_F(WaitPacketTargetTest, PacketWhoseTargetIsDestroyedReadsItAsGone)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->first));
        ASSERT_EQ(0u, this->queued());

        // The event's own reference and the association's go, so it is destroyed while the packet is armed.
        auto& events = this->emu->process.events;
        auto* wait_packet = this->emu->process.wait_completion_packets.get(this->packet);
        ASSERT_NE(nullptr, wait_packet);
        ASSERT_TRUE(events.erase(this->first));
        io_completion_wait::release_reference(this->emu->process, wait_packet->target_object_reference);
        ASSERT_EQ(nullptr, events.get(this->first));

        EXPECT_EQ(0u, this->queued());
        EXPECT_EQ(nullptr, this->emu->process.wait_completion_packets.get(this->packet)->target_object);
    }

    class ThreadAlertTest : public IoCompletionWaiterTest
    {
      protected:
        using entry_type = FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>;
        static constexpr uint64_t sentinel_key = 0xAB;
        static constexpr ULONG unwritten_count = 0xDEADBEEF;

        void SetUp() override
        {
            IoCompletionWaiterTest::SetUp();
            this->count_address = this->storage + 0x20;
        }

        NTSTATUS alert(emulator_thread& alerter, emulator_thread& target)
        {
            const auto target_handle = this->emu->process.threads.find_handle(&target);
            return this->emu->synchronize([&] { return syscalls::handle_NtAlertThread(this->context_of(alerter), target_handle); });
        }

        bool ready(emulator_thread& thread)
        {
            return this->emu->synchronize([&] { return thread.is_thread_ready(*this->emu); });
        }

        emulator_object<entry_type> entries() const
        {
            return {this->emu->memory, this->storage + 0x100};
        }

        emulator_object<ULONG> removed() const
        {
            return {this->emu->memory, this->count_address};
        }

        uint64_t first_key() const
        {
            return this->entries().read().KeyContext;
        }

        // NtRemoveIoCompletionEx for up to two entries, with the first entry's KeyContext set to sentinel_key and the count
        // to unwritten_count beforehand.
        NTSTATUS remove_ex(emulator_thread& waiter, const BOOLEAN alertable, const std::optional<handle> other_port = {})
        {
            entry_type sentinel{};
            sentinel.KeyContext = sentinel_key;
            this->entries().write(sentinel);
            this->removed().write(unwritten_count);
            return this->emu->synchronize([&] {
                return syscalls::handle_NtRemoveIoCompletionEx(this->context_of(waiter), other_port.value_or(this->port), this->entries(),
                                                               2, this->removed(), {this->emu->memory}, alertable);
            });
        }

        uint64_t count_address{};
    };

    // NtAlertThread alerts in user mode (KeAlertThread): it ends an alertable wait with STATUS_ALERTED, leaves a wait that is
    // not alertable alone, and an alertable wait that starts with the alert pending takes it before it looks at its object,
    // a signaled one included (19041 on the host).
    TEST_F(ThreadAlertTest, AnAlertEndsAnAlertableWaitOrWaitsForTheNextOne)
    {
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        const auto event_handle = this->emu->process.events.store(event{});
        const emulator_object<LARGE_INTEGER> no_timeout{this->emu->memory};
        const auto wait = [&](const BOOLEAN alertable) {
            return this->emu->synchronize(
                [&] { return syscalls::handle_NtWaitForSingleObject(this->context_of(waiter), event_handle, alertable, no_timeout); });
        };

        ASSERT_EQ(STATUS_SUCCESS, wait(TRUE));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
        EXPECT_TRUE(this->ready(waiter));
        EXPECT_EQ(STATUS_ALERTED, waiter.pending_status);
        EXPECT_FALSE(waiter.user_alerted);

        waiter.pending_status = {};
        ASSERT_EQ(STATUS_SUCCESS, wait(FALSE));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
        EXPECT_FALSE(this->ready(waiter));
        this->emu->process.events.get(event_handle)->set_signaled(true);
        EXPECT_TRUE(this->ready(waiter));
        EXPECT_EQ(STATUS_SUCCESS, waiter.pending_status);

        waiter.pending_status = {};
        EXPECT_EQ(STATUS_ALERTED, wait(TRUE));
        EXPECT_EQ(STATUS_SUCCESS, wait(TRUE));
    }

    // An alert ends an alertable NtDelayExecution with STATUS_ALERTED (19041 on the host).
    TEST_F(ThreadAlertTest, AnAlertEndsAnAlertableDelay)
    {
        constexpr int64_t ten_seconds = -100'000'000;
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        const emulator_object<LARGE_INTEGER> delay{this->emu->memory, this->storage + 0x900};
        LARGE_INTEGER interval{};
        interval.QuadPart = ten_seconds;
        delay.write(interval);

        ASSERT_EQ(STATUS_SUCCESS,
                  this->emu->synchronize([&] { return syscalls::handle_NtDelayExecution(this->context_of(waiter), TRUE, delay); }));
        EXPECT_FALSE(this->ready(waiter));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
        EXPECT_TRUE(this->ready(waiter));
        EXPECT_EQ(STATUS_ALERTED, waiter.pending_status);
    }

    // NtTestAlert takes a pending user-mode alert and finds none the next time (KeTestAlertThread; 19041 on the host).
    TEST_F(ThreadAlertTest, TestAlertTakesAPendingAlertOnce)
    {
        auto& thread = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->alert(thread, thread));
        EXPECT_EQ(STATUS_ALERTED, this->emu->synchronize([&] { return syscalls::handle_NtTestAlert(this->context_of(thread)); }));
        EXPECT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] { return syscalls::handle_NtTestAlert(this->context_of(thread)); }));
    }

    // KeAlertThread ends an alertable NtRemoveIoCompletionEx at once (KiSignalThread): KeRemoveQueueEx returns one entry,
    // STATUS_ALERTED, whose count NtRemoveIoCompletionEx stores, the entries untouched, and a packet posted right after the
    // alert stays queued. A wait that is not alertable goes on and keeps the alert pending (19041 on the host).
    TEST_F(ThreadAlertTest, AnAlertEndsAnAlertableExWaitAtOnceWithACountOfOne)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();

        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, TRUE));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, waiter));
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        EXPECT_EQ(STATUS_ALERTED, waiter.pending_status);
        EXPECT_FALSE(waiter.user_alerted);
        EXPECT_EQ(1u, this->removed().read());
        EXPECT_EQ(sentinel_key, this->first_key());
        EXPECT_EQ(1u, this->completion().queue.size());

        waiter.pending_status = {};
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, FALSE));
        EXPECT_EQ(0x11u, this->first_key());
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, FALSE));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, waiter));
        EXPECT_FALSE(this->ready(waiter));
        EXPECT_TRUE(waiter.user_alerted);
    }

    // KeRemoveQueueEx: a thread the port already has (KTHREAD.Queue, from an earlier call that got past the alert test,
    // here one that took a packet) takes what is queued before it tests for an alert, which stays pending, and with nothing
    // queued it takes the alert. A thread the port does not have tests first: the alert, a count of 1 and the packet left
    // queued, and a call that the alert ended does not join the port. Nor does the port keep a thread that joined another
    // one (19041 on the host).
    TEST_F(ThreadAlertTest, AThreadThePortHasTakesQueuedPacketsBeforeTheAlert)
    {
        auto& member = this->new_thread();
        auto& stranger = this->new_thread();
        auto& poster = this->new_thread();

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(member, FALSE));
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, member));
        EXPECT_EQ(STATUS_SUCCESS, this->remove_ex(member, TRUE));
        EXPECT_EQ(0x22u, this->first_key());
        EXPECT_EQ(1u, this->removed().read());
        EXPECT_TRUE(member.user_alerted);
        EXPECT_EQ(STATUS_ALERTED, this->remove_ex(member, TRUE));
        EXPECT_EQ(1u, this->removed().read());
        EXPECT_FALSE(member.user_alerted);

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x33));
        for (int i = 0; i < 2; ++i)
        {
            ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, stranger));
            EXPECT_EQ(STATUS_ALERTED, this->remove_ex(stranger, TRUE)) << i;
            EXPECT_EQ(1u, this->removed().read()) << i;
            EXPECT_EQ(sentinel_key, this->first_key()) << i;
        }
        EXPECT_EQ(1u, this->completion().queue.size());

        const auto other_port = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtSetIoCompletion(this->context_of(poster), other_port, 0x44, 0, STATUS_SUCCESS, 0);
        }));
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(member, FALSE, other_port));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, member));
        EXPECT_EQ(STATUS_ALERTED, this->remove_ex(member, TRUE));
        EXPECT_EQ(1u, this->completion().queue.size());
    }

    // NtRemoveIoCompletionEx stores the count under try (19041): a count it can no longer write, its page freed during the
    // wait, ends the alerted wait with STATUS_ACCESS_VIOLATION instead of failing the emulator, whether the alert ends the
    // wait at once or once the waiter resumes from a suspension (19041 on the host).
    TEST_F(ThreadAlertTest, ACountThatCannotBeStoredEndsTheAlertedWaitWithTheFault)
    {
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        for (const uint32_t suspended : {0u, 1u})
        {
            this->count_address = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, TRUE));
            ASSERT_TRUE(this->emu->memory.release_memory(this->count_address, 0));
            waiter.suspended = suspended;
            ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
            waiter.suspended = 0;
            EXPECT_TRUE(this->ready(waiter));
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, waiter.pending_status) << suspended;
            EXPECT_FALSE(waiter.user_alerted) << suspended;
            waiter.pending_status = {};
        }
    }

    // The count is a plain store under try, as a copy-out makes one (MiAccessCheck): on a writable guard page outside the
    // stacks it ends the alerted wait with STATUS_GUARD_PAGE_VIOLATION and the page loses its guard, and on a guard page the
    // waiter may not write it ends it with STATUS_ACCESS_VIOLATION and the guard stays (19041 on the host).
    TEST_F(ThreadAlertTest, ACountOnAGuardPageEndsTheAlertedWaitWithTheStoresFault)
    {
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        for (const auto& [permission, status, guard_stays] : {std::tuple{memory_permission::read_write, STATUS_GUARD_PAGE_VIOLATION, false},
                                                              std::tuple{memory_permission::read, STATUS_ACCESS_VIOLATION, true}})
        {
            this->count_address = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, TRUE));
            ASSERT_TRUE(this->emu->memory.protect_memory(this->count_address, 0x1000,
                                                         nt_memory_permission(permission, memory_permission_ext::guard)));
            ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
            EXPECT_EQ(status, waiter.pending_status);
            EXPECT_EQ(guard_stays, this->emu->memory.is_guard_page(this->count_address));
            waiter.pending_status = {};
        }
    }

    // A user APC queued to a thread in an alertable wait ends the wait at once (KiSignalThreadForApc), so an alert after it
    // finds the thread no longer waiting and stays pending (KeAlertThread): the wait returns STATUS_USER_APC once the APC
    // runs, and the alert is for the next alertable wait (19041 on the host).
    TEST_F(ThreadAlertTest, AUserApcQueuedBeforeTheAlertEndsTheWaitAndTheAlertStaysPending)
    {
        constexpr uint64_t apc_routine = 0x10000; // never run here
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        const auto event_handle = this->emu->process.events.store(event{});
        const emulator_object<LARGE_INTEGER> no_timeout{this->emu->memory};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForSingleObject(this->context_of(waiter), event_handle, TRUE, no_timeout);
        }));

        const auto waiter_handle = this->emu->process.threads.find_handle(&waiter);
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtQueueApcThread(this->context_of(alerter), waiter_handle, apc_routine, 0, 0, 0);
        }));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));

        EXPECT_FALSE(waiter.pending_status.has_value()) << "the APC ended the wait, not the alert";
        EXPECT_TRUE(waiter.user_alerted);
        EXPECT_FALSE(this->ready(waiter)) << "the alert-first check leaves the wait to the APC";
        EXPECT_TRUE(waiter.has_pending_alertable_apc());
    }

    // NtWaitForWorkViaWorkerFactory takes its work through KeRemoveQueueEx, which joins the factory's port (KiSwitchQueue): a
    // worker back from work it took there, and alerted meanwhile, takes a packet queued on that port before the alert,
    // which stays pending (19041 on the host).
    TEST_F(ThreadAlertTest, AWorkerJoinsItsFactorysPort)
    {
        auto& worker = this->new_thread();
        auto& poster = this->new_thread();
        worker_factory factory{};
        factory.io_completion = this->emu->process.io_completions.reference_of(this->port).value_or(object_reference{});
        const auto factory_handle = this->emu->process.worker_factories.store(std::move(factory));

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x11));
        const emulator_object<entry_type> packets{this->emu->memory, this->storage + 0x200};
        const emulator_object<ULONG> returned{this->emu->memory, this->storage + 0x300};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForWorkViaWorkerFactory(this->context_of(worker), factory_handle, packets, 1, returned, 0);
        }));
        ASSERT_EQ(1u, returned.read());

        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x22));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, worker));
        EXPECT_EQ(STATUS_SUCCESS, this->remove_ex(worker, TRUE));
        EXPECT_EQ(0x22u, this->first_key());
        EXPECT_TRUE(worker.user_alerted);
    }

    // KeAlertThread ends only a wait still in progress. A wait that a packet or its object's signal ended before the
    // alert has ended on Windows, even when the scheduler has not looked at it yet, so the alert stays pending (19041 on
    // the host).
    TEST_F(ThreadAlertTest, AnAlertAfterTheWaitEndedStaysPending)
    {
        auto& waiter = this->new_thread();
        auto& alerter = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, TRUE));
        ASSERT_EQ(STATUS_SUCCESS, this->post(alerter, 0x55));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
        EXPECT_EQ(STATUS_SUCCESS, waiter.pending_status);
        EXPECT_EQ(0x55u, this->first_key());
        EXPECT_TRUE(waiter.user_alerted);

        waiter.pending_status = {};
        waiter.user_alerted = false;
        const auto event_handle = this->emu->process.events.store(event{});
        const emulator_object<LARGE_INTEGER> no_timeout{this->emu->memory};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtWaitForSingleObject(this->context_of(waiter), event_handle, TRUE, no_timeout);
        }));
        this->emu->process.events.get(event_handle)->set_signaled(true);
        ASSERT_EQ(STATUS_SUCCESS, this->alert(alerter, waiter));
        EXPECT_EQ(STATUS_SUCCESS, waiter.pending_status);
        EXPECT_TRUE(waiter.user_alerted);
    }

    // A suspended waiter is not in its alertable wait: KeAlertThread keeps the alert, and once the thread resumes, its wait
    // starts over and takes the alert first, ahead of a packet queued meanwhile, with a count of 1 (19041 on the host).
    TEST_F(ThreadAlertTest, ASuspendedWaiterTakesTheAlertOnceItResumes)
    {
        auto& waiter = this->new_thread();
        auto& poster = this->new_thread();
        ASSERT_EQ(STATUS_SUCCESS, this->remove_ex(waiter, TRUE));
        waiter.suspended = 1;
        ASSERT_EQ(STATUS_SUCCESS, this->post(poster, 0x66));
        ASSERT_EQ(STATUS_SUCCESS, this->alert(poster, waiter));
        EXPECT_FALSE(waiter.pending_status.has_value());
        EXPECT_TRUE(waiter.user_alerted);

        waiter.suspended = 0;
        EXPECT_TRUE(this->ready(waiter));
        EXPECT_EQ(STATUS_ALERTED, waiter.pending_status);
        EXPECT_FALSE(waiter.user_alerted);
        EXPECT_EQ(1u, this->removed().read());
        EXPECT_EQ(sentinel_key, this->first_key());
        EXPECT_EQ(1u, this->completion().queue.size());
    }

    // KiContinueEx tests for an alert only for a nonzero inline argument, as NtContinue(context, TRUE) passes.
    // KeTestAlertThread then takes a pending user-mode alert, and only without one are queued user APCs delivered (19041 on
    // the host). The context's flags are 0, so continuing to it changes no register.
    TEST_F(ThreadAlertTest, ContinueTestsTheAlertOnlyForANonzeroArgument)
    {
        auto& thread = this->new_thread();
        auto& vcpu = this->emu->vcpu(0);
        const auto context =
            this->emu->memory.allocate_memory(static_cast<size_t>(page_align_up(sizeof(CONTEXT64))), memory_permission::read_write);
        const auto continue_with = [&](const uint64_t argument) {
            vcpu.switch_thread = false;
            thread.apc_alertable = false;
            return this->emu->synchronize([&] { return syscalls::handle_NtContinueEx(this->context_of(thread), context, argument); });
        };

        thread.user_alerted = true;
        EXPECT_EQ(STATUS_SUCCESS, continue_with(FALSE));
        EXPECT_TRUE(thread.user_alerted);
        EXPECT_FALSE(vcpu.switch_thread);

        EXPECT_EQ(STATUS_SUCCESS, continue_with(TRUE));
        EXPECT_FALSE(thread.user_alerted);
        EXPECT_FALSE(vcpu.switch_thread);

        EXPECT_EQ(STATUS_SUCCESS, continue_with(TRUE));
        EXPECT_TRUE(vcpu.switch_thread);
        EXPECT_TRUE(thread.apc_alertable);
    }
}
