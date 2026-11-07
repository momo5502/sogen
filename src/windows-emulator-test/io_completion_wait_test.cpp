#include "emulation_test_utils.hpp"
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>

#include <gtest/gtest.h>

namespace sogen::syscalls
{
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
            target_event.signaled = c.signal_before_cancel;
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
                target_event.signaled = true;
            }

            ULONG packets = 0;
            while (io_completion_wait::dequeue_io_completion_message(*this->emu, this->completion, message))
            {
                ++packets;
            }
            EXPECT_EQ(packets, c.packets_after);
            EXPECT_EQ(target_event.signaled, !c.signal_before_cancel);

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

    // The resumed waiter takes the queued packet before it is back in the wait list, so the next worker factory release,
    // which taking the first one queues, stays queued for another wait instead of reaching the same one and being lost.
    TEST_F(IoCompletionWaiterTest, ResumedWorkerTakesOneReleaseAndLeavesTheNextQueued)
    {
        auto& worker = this->new_thread();
        auto& releaser = this->new_thread();
        worker_factory factory{};
        factory.io_completion_handle = this->port;
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

        this->emu->process.events.get(target)->signaled = true;
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
                this->emu->process.events.get(target)->signaled = true;
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
        ASSERT_TRUE(events.erase(this->first));
        ASSERT_TRUE(events.erase(this->first));
        ASSERT_EQ(nullptr, events.get(this->first));

        EXPECT_EQ(0u, this->queued());
        EXPECT_EQ(nullptr, this->emu->process.wait_completion_packets.get(this->packet)->target_object);
    }
}
