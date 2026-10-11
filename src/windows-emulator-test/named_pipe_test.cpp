#include <backend_selection.hpp>
#include <devices/named_pipe.hpp>
#include <emulator_utils.hpp>
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, handle h, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
}

namespace sogen::test
{
    namespace
    {
        class NamedPipeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(),
                                                               emulator_settings{.disable_logging = true, .load_registry = false},
                                                               emulator_callbacks{}, std::move(interfaces));
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                this->storage = this->emu->memory.allocate_memory(0x50000, memory_permission::read_write);
                auto pair = utils::host_pipe::create_pair(false, 4096);
                this->reader = this->store_pipe(std::move(pair.first), FILE_READ_DATA);
                this->writer = this->store_pipe(std::move(pair.second), FILE_WRITE_DATA);
            }

            handle store_pipe(std::shared_ptr<utils::host_pipe> endpoint, ACCESS_MASK access)
            {
                io_device_container container{u"NamedPipe", *this->emu, {}};
                auto* pipe = container.get_internal_device<named_pipe>();
                pipe->endpoint = std::move(endpoint);
                pipe->access = access;
                return this->emu->process.devices.store(std::move(container));
            }

            named_pipe& pipe(handle h)
            {
                return *this->emu->process.devices.get(h)->get_internal_device<named_pipe>();
            }

            io_device_context request(handle h, uint32_t length)
            {
                io_device_context context{this->emu->memory};
                context.source_handle = h;
                context.io_status_block.set_address(this->storage + (h == this->reader ? 0 : 0x100));
                context.io_status_block.write({});
                context.output_buffer = this->storage + 0x1000;
                context.output_buffer_length = length;
                context.input_buffer = this->storage + 0x21000;
                context.input_buffer_length = length;
                context.vcpu = &this->emu->vcpu(0);
                return this->emu->process.devices.get(h)->prepare_io_context(context);
            }

            NTSTATUS write(std::string_view data)
            {
                this->emu->memory.write_memory(this->storage + 0x21000, data.data(), data.size());
                return this->pipe(this->writer)
                    .start_io(*this->emu, this->request(this->writer, static_cast<uint32_t>(data.size())), false);
            }

            NTSTATUS close(handle h)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtClose(context, h);
            }

            std::string read_output(size_t size)
            {
                std::string data(size, '\0');
                this->emu->memory.read_memory(this->storage + 0x1000, data.data(), data.size());
                return data;
            }

            std::unique_ptr<windows_emulator> emu;
            handle reader{};
            handle writer{};
            uint64_t storage{};
        };
    }

    TEST_F(NamedPipeTest, PendingReadCompletesWithEventAndCompletionPort)
    {
        const auto completion = this->emu->process.io_completions.store(io_completion{});
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(this->emu->process.devices.get(this->reader)
                      ->set_completion_association(this->emu->process, this->emu->vcpu(0).active_thread, completion, 0x1234),
                  STATUS_SUCCESS);
        auto context = this->request(this->reader, 128);
        context.event = event;
        context.apc_context = 0x5678;
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_PENDING);
        EXPECT_FALSE(this->emu->process.events.get(event)->signaled);
        ASSERT_EQ(this->write("hello pipe"), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(context.io_status_block.read().Information, 10u);
        EXPECT_TRUE(this->emu->process.events.get(event)->signaled);
        EXPECT_EQ(this->read_output(10), "hello pipe");
        io_completion_message message{};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
        EXPECT_EQ(message.key_context, 0x1234u);
        EXPECT_EQ(message.apc_context, 0x5678u);
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
    }

    TEST_F(NamedPipeTest, PartialReadsConsumeOnlyReturnedBytes)
    {
        ASSERT_EQ(this->write("abcdef"), STATUS_SUCCESS);
        auto context = this->request(this->reader, 2);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_SUCCESS);
        EXPECT_EQ(this->read_output(2), "ab");
        context = this->request(this->reader, 16);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_SUCCESS);
        EXPECT_EQ(context.io_status_block.read().Information, 4u);
        EXPECT_EQ(this->read_output(4), "cdef");
    }

    TEST_F(NamedPipeTest, LastWriterClosureCompletesReadWithBrokenPipe)
    {
        auto duplicate = this->pipe(this->writer).endpoint->duplicate();
        auto context = this->request(this->reader, 16);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_PENDING);
        ASSERT_EQ(this->close(this->writer), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_PENDING);
        duplicate.reset();
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_PIPE_BROKEN);
    }

    TEST_F(NamedPipeTest, CancelledReadSignalsItsEvent)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        auto context = this->request(this->reader, 16);
        context.event = event;
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_PENDING);
        ASSERT_TRUE(this->pipe(this->reader).cancel_io(*this->emu, context.io_status_block.value()));
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_CANCELLED);
        EXPECT_TRUE(this->emu->process.events.get(event)->signaled);
    }

    TEST_F(NamedPipeTest, SynchronousReadParksOnlyItsIssuingGuestThread)
    {
        this->pipe(this->reader).create_options = FILE_SYNCHRONOUS_IO_NONALERT;
        auto context = this->request(this->reader, 16);
        const auto status = this->emu->synchronize([&] { return this->pipe(this->reader).start_io(*this->emu, context, true); });
        ASSERT_EQ(status, STATUS_PENDING);
        EXPECT_EQ(this->emu->vcpu(0).thread().await_file_io, context.io_status_block.value());
        ASSERT_EQ(this->write("ready"), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_SUCCESS);
    }

    TEST_F(NamedPipeTest, WaitingOnPipeHandleObservesPendingIoCompletion)
    {
        auto context = this->request(this->reader, 16);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_PENDING);
        const syscall_context syscall{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        const auto status = this->emu->synchronize(
            [&] { return syscalls::handle_NtWaitForSingleObject(syscall, this->reader, FALSE, {this->emu->memory}); });
        ASSERT_EQ(status, STATUS_SUCCESS);
        EXPECT_FALSE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
        ASSERT_EQ(this->write("ready"), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_SUCCESS);
    }

    TEST_F(NamedPipeTest, PipeDirectoryAcceptsDosAndDeviceAliases)
    {
        const syscall_context syscall{
            .win_emu = *this->emu, .emu = this->emu->emu(), .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        for (const std::u16string_view alias : {u"\\??\\PIPE\\", u"\\Device\\NamedPipe\\", u"\\??\\pipe\\"})
        {
            emulator_allocator allocator{this->emu->memory, this->storage + 0x30000, 0x10000};
            const auto name = allocator.make_unicode_string(alias);
            const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
            attributes.write({.Length = sizeof(OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>), .ObjectName = name.value()});
            const auto output = allocator.reserve<handle>();
            const auto iosb = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
            ASSERT_EQ(syscalls::handle_NtOpenFile(syscall, output, GENERIC_READ, attributes, iosb, 3, 0), STATUS_SUCCESS);
            EXPECT_EQ(this->pipe(output.read()).name, u"\\Device\\NamedPipe\\");
            EXPECT_FALSE(this->pipe(output.read()).endpoint);
            EXPECT_EQ(this->close(output.read()), STATUS_SUCCESS);
        }
    }
}
