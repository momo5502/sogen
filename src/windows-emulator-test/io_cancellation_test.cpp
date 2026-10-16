#include "emulation_test_utils.hpp"
#include <devices/afd_types.hpp>
#include <filesystem/host_directory_provider.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
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
                interfaces.socket_factory = network::create_static_socket_factory();
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

            NTSTATUS query_io_pending(handle target, uint64_t buffer, uint32_t length = sizeof(ULONG), uint64_t return_buffer = 0)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtQueryInformationThread(context, target, ThreadIsIoPending, buffer, length,
                                                                 {this->emu->memory, return_buffer});
            }

            void expect_io_pending(handle target, bool expected)
            {
                this->emu->memory.write_memory<uint64_t>(this->storage + 0xE00, 0xAAAAAAAAAAAAAAAA);
                this->emu->memory.write_memory<uint64_t>(this->storage + 0xE10, 0xBBBBBBBBBBBBBBBB);
                ASSERT_EQ(STATUS_SUCCESS, this->query_io_pending(target, this->storage + 0xE00, sizeof(ULONG), this->storage + 0xE10));
                EXPECT_EQ(static_cast<ULONG>(expected), this->emu->memory.read_memory<ULONG>(this->storage + 0xE00));
                EXPECT_EQ(0xAAAAAAAAu, this->emu->memory.read_memory<ULONG>(this->storage + 0xE04));
                EXPECT_EQ(sizeof(ULONG), this->emu->memory.read_memory<ULONG>(this->storage + 0xE10));
                EXPECT_EQ(0xBBBBBBBBu, this->emu->memory.read_memory<ULONG>(this->storage + 0xE14));
            }

            std::unique_ptr<windows_emulator> emu{};
            handle first{};
            handle second{};
            handle endpoint{};
            uint64_t storage{};
        };
    }

    TEST_F(IoCancellationTest, IdleThreadsHaveNoPendingIo)
    {
        this->expect_io_pending(CURRENT_THREAD, false);
        this->expect_io_pending(this->first, false);
        this->expect_io_pending(this->second, false);
    }

    TEST_F(IoCancellationTest, PendingIoQueryTracksIssuersAndCancellation)
    {
        this->queue_poll(0, this->first);
        this->expect_io_pending(this->first, true);
        this->expect_io_pending(this->second, false);
        this->queue_poll(0x100, this->second);
        this->queue_poll(0x200, this->first);
        this->expect_io_pending(CURRENT_THREAD, true);
        this->expect_io_pending(this->second, true);
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelIoFileEx(context, this->endpoint, this->status(0), this->status(0xF00)));
        this->expect_io_pending(this->first, true);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        this->expect_io_pending(this->first, false);
        this->expect_io_pending(this->second, true);
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(this->second);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        this->expect_io_pending(CURRENT_THREAD, false);
    }

    TEST_F(IoCancellationTest, EndpointPendingIoClearsOnCompletion)
    {
        struct endpoint_creation_data
        {
            uint64_t flags{};
            std::array<char, 16> packet{};
            uint64_t group{};
            int32_t address_family{2};
            int32_t type{1};
            int32_t protocol{6};
        };

        const endpoint_creation_data creation{};
        this->emu->memory.write_memory(this->storage + 0xD00, &creation, sizeof(creation));
        this->endpoint = this->emu->process.devices.store(
            io_device_container{u"Afd\\Endpoint", *this->emu, {.buffer = this->storage + 0xD00, .length = sizeof(creation)}});
        this->queue_poll(0, this->first);
        this->expect_io_pending(this->first, true);
        this->expect_io_pending(this->second, false);
        ASSERT_EQ(STATUS_SUCCESS, this->cancel(this->endpoint));
        this->expect_io_pending(this->first, false);
    }

    TEST_F(IoCancellationTest, PendingIoQueryRequiresExactUlongLength)
    {
        for (const uint32_t length : {0, 1, 3, 5, 8})
        {
            this->emu->memory.write_memory<ULONG>(this->storage + 0xE00, 0xAAAAAAAA);
            this->emu->memory.write_memory<ULONG>(this->storage + 0xE10, 0xBBBBBBBB);
            EXPECT_EQ(STATUS_INFO_LENGTH_MISMATCH,
                      this->query_io_pending(CURRENT_THREAD, this->storage + 0xE00, length, this->storage + 0xE10));
            EXPECT_EQ(0xAAAAAAAAu, this->emu->memory.read_memory<ULONG>(this->storage + 0xE00));
            EXPECT_EQ(0xBBBBBBBBu, this->emu->memory.read_memory<ULONG>(this->storage + 0xE10));
        }
    }

    TEST_F(IoCancellationTest, PendingIoQueryValidatesHandlesAndBuffers)
    {
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->query_io_pending(NULL_HANDLE, this->storage + 0xE00));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_io_pending(CURRENT_THREAD, 0));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_io_pending(CURRENT_THREAD, 0x12340000));
        EXPECT_EQ(STATUS_DATATYPE_MISALIGNMENT, this->query_io_pending(CURRENT_THREAD, this->storage + 0xE01));
        EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->query_io_pending(CURRENT_THREAD, this->storage + 0xE00, sizeof(ULONG), 0x12340000));
    }

    TEST_F(IoCancellationTest, PendingIoQueryAllowsOmittedReturnLength)
    {
        EXPECT_EQ(STATUS_SUCCESS, this->query_io_pending(CURRENT_THREAD, this->storage + 0xE00));
        EXPECT_EQ(0u, this->emu->memory.read_memory<ULONG>(this->storage + 0xE00));
    }

    TEST_F(IoCancellationTest, Wow64PendingIoQueryWritesOnlyUlong)
    {
        this->emu->process.is_wow64_process = true;
        this->expect_io_pending(CURRENT_THREAD, false);
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
        entry.handle = guest_file{this->emu->file_sys, windows_path{'c', {u"cancel.tmp"}}, u"w+b", std::make_unique<host_file>(tmpfile())};
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
