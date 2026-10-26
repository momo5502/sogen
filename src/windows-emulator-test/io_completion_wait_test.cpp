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
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
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

    TEST_F(ConsoleWaitCompletionTest, RealFileHandleStaysUnsupported)
    {
        const auto file_handle = this->emu->process.files.store(file{});
        EXPECT_EQ(this->associate(file_handle), STATUS_OBJECT_TYPE_MISMATCH);
    }
}
