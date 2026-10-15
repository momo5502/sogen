#include "emulation_test_utils.hpp"
#include <devices/afd_types.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCancelIoFile(const syscall_context& c, handle file_handle,
                                   emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
    NTSTATUS handle_NtCancelIoFileEx(const syscall_context& c, handle file_handle,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_request_to_cancel,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
}

namespace sogen::test
{
    namespace
    {
        class IoCancellationTest : public testing::Test
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
                this->first =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->second =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 12, false});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->first);
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                this->endpoint = this->emu->process.devices.store(io_device_container{u"Afd\\Mio", *this->emu, {}});
            }

            auto status(uint64_t offset)
            {
                return emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>{this->emu->memory, this->storage + offset};
            }

            void queue_poll(uint64_t offset, handle thread)
            {
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                AFD_POLL_INFO<EmulatorTraits<Emu64>> info{};
                info.Timeout.QuadPart = std::numeric_limits<int64_t>::max();
                this->emu->memory.write_memory(this->storage + offset + 0x40, &info, sizeof(info));
                io_device_context request{this->emu->memory};
                request.source_handle = this->endpoint;
                request.io_control_code = 0x12024;
                request.io_status_block = this->status(offset);
                request.input_buffer = this->storage + offset + 0x40;
                request.output_buffer = request.input_buffer;
                request.input_buffer_length = sizeof(info);
                request.output_buffer_length = sizeof(info);
                request.vcpu = &this->emu->vcpu(0);
                ASSERT_EQ(STATUS_PENDING, this->emu->process.devices.get(this->endpoint)->execute_ioctl(*this->emu, request));
            }

            NTSTATUS cancel(handle target)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtCancelIoFile(context, target, this->status(0xF00));
            }

            std::unique_ptr<windows_emulator> emu{};
            handle first{};
            handle second{};
            handle endpoint{};
            uint64_t storage{};
        };
    }

    TEST_F(IoCancellationTest, LegacyCancellationOnlyCancelsCallingThreadsPolls)
    {
        this->queue_poll(0, this->first);
        this->queue_poll(0x100, this->second);
        this->queue_poll(0x200, this->first);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        EXPECT_EQ(STATUS_CANCELLED, this->status(0).read().Status);
        EXPECT_EQ(STATUS_PENDING, this->status(0x100).read().Status);
        EXPECT_EQ(STATUS_CANCELLED, this->status(0x200).read().Status);
        EXPECT_EQ(STATUS_SUCCESS, this->status(0xF00).read().Status);
        EXPECT_EQ(0u, this->status(0xF00).read().Information);
        EXPECT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->second);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        EXPECT_EQ(STATUS_CANCELLED, this->status(0x100).read().Status);
    }

    TEST_F(IoCancellationTest, ExtendedCancellationCanCancelAllThreadsPolls)
    {
        this->queue_poll(0, this->first);
        this->queue_poll(0x100, this->second);
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelIoFileEx(context, this->endpoint, {this->emu->memory}, this->status(0xF00)));
        EXPECT_EQ(STATUS_CANCELLED, this->status(0).read().Status);
        EXPECT_EQ(STATUS_CANCELLED, this->status(0x100).read().Status);
    }

    TEST_F(IoCancellationTest, InvalidHandlesAreRejected)
    {
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->cancel(NULL_HANDLE));
    }

    TEST_F(IoCancellationTest, MissingStatusBlockIsRejected)
    {
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, syscalls::handle_NtCancelIoFile(context, this->endpoint, {this->emu->memory}));
    }

    TEST_F(IoCancellationTest, CompletedFileIoNeedsNoCancellation)
    {
        file entry{};
        entry.handle = tmpfile();
        ASSERT_TRUE(entry.handle);
        const auto target = this->emu->process.files.store(std::move(entry));
        EXPECT_EQ(STATUS_SUCCESS, this->cancel(target));
    }

    TEST_F(IoCancellationTest, Wow64StatusDoesNotOverwriteFollowingMemory)
    {
        this->emu->process.is_wow64_process = true;
        this->emu->memory.write_memory<uint64_t>(this->storage + 0xF08, 0x123456789ABCDEF0);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(STDIN_HANDLE));
        EXPECT_EQ(0x123456789ABCDEF0u, this->emu->memory.read_memory<uint64_t>(this->storage + 0xF08));
    }

    TEST_F(IoCancellationTest, IssuingThreadSurvivesContextSerialization)
    {
        io_device_context request{this->emu->memory};
        request.issuing_thread_id = 12;
        utils::buffer_serializer snapshot{};
        request.serialize(snapshot);
        utils::buffer_deserializer reader{snapshot};
        io_device_context restored{this->emu->memory};
        restored.deserialize(reader);
        EXPECT_EQ(12u, restored.issuing_thread_id);
    }
}
