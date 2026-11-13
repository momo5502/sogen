#include "emulation_test_utils.hpp"
#include <devices/named_pipe.hpp>
#include <emulator_utils.hpp>
#include <syscall_utils.hpp>
#include <limits>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtCreateNamedPipeFile(const syscall_context& c, emulator_object<handle> file_handle, ULONG desired_access,
                                          emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                          emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                                          ULONG create_disposition, ULONG create_options, ULONG named_pipe_type, ULONG read_mode,
                                          ULONG completion_mode, ULONG maximum_instances, ULONG inbound_quota, ULONG outbound_quota,
                                          emulator_object<LARGE_INTEGER> default_timeout);
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
    NTSTATUS handle_NtFsControlFile(const syscall_context& c, handle file_handle, handle event, emulator_pointer apc_routine,
                                    emulator_pointer apc_context, emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                    ULONG fs_control_code, emulator_pointer input_buffer, ULONG input_buffer_length,
                                    emulator_pointer output_buffer, ULONG output_buffer_length);
    NTSTATUS handle_NtReadFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                               emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtWriteFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                                emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                                emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtCancelIoFile(const syscall_context& c, handle file_handle,
                                   emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view pipe_name = u"\\Device\\NamedPipe\\disconnect";
        constexpr ULONG message_type = 1;
        constexpr ULONG byte_type = 0;
        constexpr ULONG synchronous = FILE_SYNCHRONOUS_IO_NONALERT;
        constexpr ULONG asynchronous = 0;
        constexpr ULONG large_write = 0x10000;

        class NamedPipeDisconnectTest : public testing::Test
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
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
                this->storage = this->emu->memory.allocate_memory(0x30000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb(const uint64_t offset = 0x100) const
            {
                return {this->emu->memory, this->storage + offset};
            }

            emulator_object<handle> output() const
            {
                return {this->emu->memory, this->storage + 0x200};
            }

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> attributes(const std::u16string_view name)
            {
                emulator_allocator allocator{this->emu->memory, this->storage + 0x1000, 0x1000};
                const auto string = allocator.make_unicode_string(name);
                const auto object = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.ObjectName = string.value();
                object.write(values);
                return object;
            }

            handle create_server(const ULONG type = message_type, const std::u16string_view name = pipe_name)
            {
                const emulator_object<LARGE_INTEGER> default_timeout{this->emu->memory, this->storage + 0x300};
                default_timeout.write({.QuadPart = -400LL * 10000});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateNamedPipeFile(
                                              this->context(), this->output(), GENERIC_READ | GENERIC_WRITE, this->attributes(name),
                                              this->iosb(0x180), 3, 3, 0, type, 0, 0, 1, 4096, 4096, default_timeout));
                return this->output().read();
            }

            NTSTATUS try_open(const ULONG options, const std::u16string_view name = pipe_name)
            {
                return syscalls::handle_NtOpenFile(this->context(), this->output(), GENERIC_READ | GENERIC_WRITE, this->attributes(name),
                                                   this->iosb(0x180), 3, options);
            }

            handle open_client(const ULONG options = synchronous, const std::u16string_view name = pipe_name)
            {
                EXPECT_EQ(STATUS_SUCCESS, this->try_open(options, name));
                return this->output().read();
            }

            NTSTATUS fsctl(const handle file, const ULONG code, const uint64_t iosb_offset = 0x100, const uint64_t output_buffer = 0,
                           const ULONG output_length = 0)
            {
                this->iosb(iosb_offset).write({.Status = static_cast<NTSTATUS>(0xAAAAAAAA), .Information = 0xAAAAAAAA});
                return syscalls::handle_NtFsControlFile(this->context(), file, {}, 0, 0, this->iosb(iosb_offset), code, 0, 0, output_buffer,
                                                        output_length);
            }

            NTSTATUS disconnect(const handle file, const uint64_t iosb_offset = 0x100)
            {
                return this->fsctl(file, FSCTL_PIPE_DISCONNECT, iosb_offset);
            }

            NTSTATUS write(const handle file, const std::string_view data, const uint64_t iosb_offset = 0x100)
            {
                const auto buffer = this->storage + 0x2000;
                this->emu->memory.write_memory(buffer, data.data(), data.size());
                return syscalls::handle_NtWriteFile(this->context(), file, 0, 0, 0, this->iosb(iosb_offset), buffer,
                                                    static_cast<ULONG>(data.size()), {this->emu->memory}, {this->emu->memory});
            }

            NTSTATUS write_large(const handle file, const uint64_t iosb_offset)
            {
                return syscalls::handle_NtWriteFile(this->context(), file, 0, 0, 0, this->iosb(iosb_offset), this->storage + 0x10000,
                                                    large_write, {this->emu->memory}, {this->emu->memory});
            }

            NTSTATUS read(const handle file, const uint64_t iosb_offset = 0x100)
            {
                return syscalls::handle_NtReadFile(this->context(), file, 0, 0, 0, this->iosb(iosb_offset), this->read_buffer(), 64,
                                                   {this->emu->memory}, {this->emu->memory});
            }

            uint64_t read_buffer() const
            {
                return this->storage + 0x3000;
            }

            std::string read_data(const size_t size) const
            {
                std::string data(size, '\0');
                this->emu->memory.read_memory(this->read_buffer(), data.data(), data.size());
                return data;
            }

            NTSTATUS peek(const handle file)
            {
                return this->fsctl(file, FSCTL_PIPE_PEEK, 0x100, this->storage + 0x4000, sizeof(file_pipe_peek_buffer));
            }

            NTSTATUS cancel(const handle file)
            {
                return syscalls::handle_NtCancelIoFile(this->context(), file, this->iosb(0x700));
            }

            NTSTATUS close(const handle file)
            {
                return syscalls::handle_NtClose(this->context(), file);
            }

            NTSTATUS wait_for_pipe(const handle directory, const std::u16string_view leaf, const uint64_t iosb_offset)
            {
                const auto input = this->storage + 0x5000;
                const auto name_bytes = static_cast<ULONG>(leaf.size() * sizeof(char16_t));
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, timeout),
                                               LARGE_INTEGER{.QuadPart = std::numeric_limits<LONGLONG>::min()});
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, name_length), name_bytes);
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, timeout_specified), static_cast<BOOLEAN>(TRUE));
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, name), leaf.data(), name_bytes);
                return syscalls::handle_NtFsControlFile(this->context(), directory, {}, 0, 0, this->iosb(iosb_offset), FSCTL_PIPE_WAIT,
                                                        input, static_cast<ULONG>(offsetof(file_pipe_wait_for_buffer, name)) + name_bytes,
                                                        0, 0);
            }

            void work(const handle file)
            {
                this->emu->process.devices.get(file)->work(*this->emu);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
        };
    }

    TEST_F(NamedPipeDisconnectTest, InstanceThatNeverHadAClientDisconnects)
    {
        const auto server = this->create_server();
        this->disconnect(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
    }

    TEST_F(NamedPipeDisconnectTest, DisconnectedInstanceRejectsBothEnds)
    {
        const auto server = this->create_server();
        const auto client = this->open_client();
        ASSERT_EQ(STATUS_SUCCESS, this->write(client, "c2s"));
        ASSERT_EQ(STATUS_SUCCESS, this->write(server, "s2c"));
        this->disconnect(server);
        ASSERT_EQ(STATUS_SUCCESS, this->iosb().read().Status);

        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->write(client, "x"));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->read(client));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->peek(client));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->read(server));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->write(server, "x"));
        EXPECT_EQ(STATUS_INVALID_PIPE_STATE, this->peek(server));

        this->disconnect(server);
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->iosb().read().Status);
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->disconnect(client));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->iosb().read().Status);
    }

    TEST_F(NamedPipeDisconnectTest, DisconnectThroughAConnectedClientIsRejected)
    {
        const auto server = this->create_server();
        const auto client = this->open_client();
        EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->disconnect(client));
        EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->iosb().read().Status);
        ASSERT_EQ(STATUS_SUCCESS, this->write(client, "alive"));
        EXPECT_EQ(STATUS_SUCCESS, this->read(server));
        EXPECT_EQ("alive", this->read_data(5));
    }

    TEST_F(NamedPipeDisconnectTest, DisconnectOnThePipeDirectoryReportsDisconnected)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->try_open(synchronous, u"\\Device\\NamedPipe\\"));
        const auto directory = this->output().read();
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->disconnect(directory));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->iosb().read().Status);
    }

    TEST_F(NamedPipeDisconnectTest, PendingConnectEndsWithTheDisconnect)
    {
        const auto server = this->create_server();
        constexpr uint64_t listen_iosb = 0x400;
        ASSERT_EQ(STATUS_PENDING, this->fsctl(server, FSCTL_PIPE_LISTEN, listen_iosb));
        this->disconnect(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->iosb(listen_iosb).read().Status);
        EXPECT_EQ(0u, this->iosb(listen_iosb).read().Information);
    }

    TEST_F(NamedPipeDisconnectTest, QueuedWaitOnTheServerEndKeepsItsOwnResult)
    {
        const auto server = this->create_server();
        this->open_client();
        constexpr uint64_t wait_iosb = 0x600;
        ASSERT_EQ(STATUS_PENDING, this->wait_for_pipe(server, u"disconnect", wait_iosb));
        this->disconnect(server);
        ASSERT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
        EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->iosb(wait_iosb).read().Status);
        EXPECT_EQ(0u, this->iosb(wait_iosb).read().Information);
    }

    TEST_F(NamedPipeDisconnectTest, PendingReadsAndWritesEndWithTheDisconnect)
    {
        constexpr uint64_t server_read = 0x400;
        constexpr uint64_t server_write = 0x480;
        constexpr uint64_t client_read = 0x500;
        constexpr uint64_t client_write = 0x580;
        for (const bool server_side : {true, false})
        {
            const auto name = server_side ? std::u16string_view{u"\\Device\\NamedPipe\\server-io"}
                                          : std::u16string_view{u"\\Device\\NamedPipe\\client-io"};
            const auto server = this->create_server(byte_type, name);
            const auto client = this->open_client(asynchronous, name);
            const auto reader = server_side ? server : client;
            const auto read_iosb = server_side ? server_read : client_read;
            const auto write_iosb = server_side ? server_write : client_write;
            ASSERT_EQ(STATUS_PENDING, this->read(reader, read_iosb));
            ASSERT_EQ(STATUS_PENDING, this->write_large(reader, write_iosb));

            this->disconnect(server);
            ASSERT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
            this->cancel(reader);
            for (const auto offset : {read_iosb, write_iosb})
            {
                EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->iosb(offset).read().Status);
                EXPECT_EQ(0u, this->iosb(offset).read().Information);
            }
        }
    }

    TEST_F(NamedPipeDisconnectTest, ListenAfterDisconnectServesANewClient)
    {
        const auto server = this->create_server();
        const auto first = this->open_client();
        ASSERT_EQ(STATUS_SUCCESS, this->write(first, "stale"));
        ASSERT_EQ(STATUS_SUCCESS, this->write(server, "old"));
        this->disconnect(server);
        EXPECT_EQ(STATUS_PIPE_NOT_AVAILABLE, this->try_open(synchronous));

        constexpr uint64_t listen_iosb = 0x400;
        ASSERT_EQ(STATUS_PENDING, this->fsctl(server, FSCTL_PIPE_LISTEN, listen_iosb));
        const auto second = this->open_client(asynchronous);
        this->work(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb(listen_iosb).read().Status);

        constexpr uint64_t server_read = 0x500;
        constexpr uint64_t client_read = 0x580;
        ASSERT_EQ(STATUS_PENDING, this->read(server, server_read));
        ASSERT_EQ(STATUS_PENDING, this->read(second, client_read));
        ASSERT_EQ(STATUS_SUCCESS, this->write(second, "hello"));
        this->work(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb(server_read).read().Status);
        EXPECT_EQ(5u, this->iosb(server_read).read().Information);
        EXPECT_EQ("hello", this->read_data(5));

        ASSERT_EQ(STATUS_SUCCESS, this->write(server, "world"));
        this->work(second);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb(client_read).read().Status);
        EXPECT_EQ("world", this->read_data(5));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->write(first, "x"));
    }

    TEST_F(NamedPipeDisconnectTest, InstanceWhoseClientClosedDisconnectsAndListensAgain)
    {
        const auto server = this->create_server();
        const auto client = this->open_client();
        ASSERT_EQ(STATUS_SUCCESS, this->close(client));
        this->disconnect(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);

        constexpr uint64_t listen_iosb = 0x400;
        ASSERT_EQ(STATUS_PENDING, this->fsctl(server, FSCTL_PIPE_LISTEN, listen_iosb));
        this->open_client();
        this->work(server);
        EXPECT_EQ(STATUS_SUCCESS, this->iosb(listen_iosb).read().Status);
    }

    TEST_F(NamedPipeDisconnectTest, ClosingTheDisconnectedServerRemovesTheName)
    {
        const auto server = this->create_server();
        const auto client = this->open_client();
        this->disconnect(server);
        ASSERT_EQ(STATUS_SUCCESS, this->close(server));

        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->write(client, "x"));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->read(client));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->peek(client));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->disconnect(client));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->try_open(synchronous));
    }

    TEST_F(NamedPipeDisconnectTest, SnapshotPreservesDisconnectedState)
    {
        const auto server = this->create_server();
        const auto client = this->open_client();
        this->disconnect(server);
        utils::buffer_serializer snapshot;
        this->emu->process.devices.get(server)->serialize(snapshot);
        this->emu->process.devices.get(client)->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        input.register_factory<guest_machine_wrapper>([&] { return guest_machine_wrapper{this->emu->emu()}; });
        input.register_factory<windows_emulator_wrapper>([&] { return windows_emulator_wrapper{*this->emu}; });
        this->emu->process.devices.get(server)->deserialize(input);
        this->emu->process.devices.get(client)->deserialize(input);

        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->write(client, "x"));
        EXPECT_EQ(STATUS_PIPE_DISCONNECTED, this->read(server));
        EXPECT_EQ(STATUS_PENDING, this->fsctl(server, FSCTL_PIPE_LISTEN));
    }

    TEST_F(NamedPipeDisconnectTest, ListeningAgainCompletesAPendingWait)
    {
        const auto server = this->create_server();
        this->open_client();
        ASSERT_EQ(STATUS_SUCCESS, this->try_open(asynchronous, u"\\Device\\NamedPipe\\"));
        const auto directory = this->output().read();
        constexpr uint64_t wait_iosb = 0x600;
        ASSERT_EQ(STATUS_PENDING, this->wait_for_pipe(directory, u"disconnect", wait_iosb));
        this->disconnect(server);
        this->work(directory);
        EXPECT_EQ(STATUS_PENDING, this->iosb(wait_iosb).read().Status);

        constexpr uint64_t listen_iosb = 0x400;
        ASSERT_EQ(STATUS_PENDING, this->fsctl(server, FSCTL_PIPE_LISTEN, listen_iosb));
        EXPECT_EQ(STATUS_SUCCESS, this->iosb(wait_iosb).read().Status);
    }
}
