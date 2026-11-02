#include "emulation_test_utils.hpp"
#include <devices/named_pipe.hpp>
#include <emulator_utils.hpp>
#include <io_completion_wait.hpp>
#include <syscall_utils.hpp>
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
    NTSTATUS handle_NtSetEvent(const syscall_context& c, uint64_t handle, emulator_object<LONG> previous_state);
    NTSTATUS handle_NtQueryEvent(const syscall_context& c, handle event_handle, uint32_t event_information_class,
                                 emulator_object<EVENT_BASIC_INFORMATION> event_information, uint32_t event_information_length,
                                 emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr NTSTATUS iosb_sentinel = static_cast<NTSTATUS>(0xAAAAAAAA);
        constexpr uint64_t completion_key = 0x77;
        constexpr uint64_t completion_context = 0x5678;
        constexpr std::u16string_view listening_name = u"listening";
        constexpr auto wait_header = static_cast<ULONG>(offsetof(file_pipe_wait_for_buffer, name));
        constexpr auto listening_name_bytes = static_cast<ULONG>(listening_name.size() * sizeof(char16_t));
        constexpr ULONG full_wait_request = wait_header + listening_name_bytes;
        constexpr ULONG name_bytes_before_the_edge = 6;

        struct buffer_case
        {
            uint64_t address;
            ULONG length;
        };

        struct wait_case
        {
            buffer_case input;
            std::optional<ULONG> name_bytes_in_memory{};
        };

        struct pipe_writer
        {
            bool server_writes;
            bool synchronous;
        };

        constexpr std::array all_writers{
            pipe_writer{.server_writes = true, .synchronous = true}, pipe_writer{.server_writes = true, .synchronous = false},
            pipe_writer{.server_writes = false, .synchronous = true}, pipe_writer{.server_writes = false, .synchronous = false}};

        class NamedPipeRequestBufferTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                const auto edge_region = this->emu->memory.allocate_memory(0x2000, memory_permission::read_write, true);
                ASSERT_TRUE(this->emu->memory.commit_memory(edge_region, 0x1000, memory_permission::read_write));
                this->edge = edge_region + 0x1000;
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
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

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb() const
            {
                return {this->emu->memory, this->storage + 0x100};
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> read_iosb() const
            {
                return {this->emu->memory, this->storage + 0x140};
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> setup_iosb() const
            {
                return {this->emu->memory, this->storage + 0x180};
            }

            emulator_object<handle> output() const
            {
                return {this->emu->memory, this->storage + 0x200};
            }

            uint64_t read_buffer() const
            {
                return this->storage + 0x3000;
            }

            handle create_server(const std::u16string_view name, const ULONG create_options = 0)
            {
                const emulator_object<LARGE_INTEGER> default_timeout{this->emu->memory, this->storage + 0x300};
                default_timeout.write({.QuadPart = -400LL * 10000});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateNamedPipeFile(
                                              this->context(), this->output(), GENERIC_READ | GENERIC_WRITE, this->attributes(name),
                                              this->setup_iosb(), 3, 3, create_options, 0, 0, 0, 1, 4096, 4096, default_timeout));
                return this->output().read();
            }

            handle open(const std::u16string_view name, const ULONG options = 0)
            {
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtOpenFile(this->context(), this->output(), GENERIC_READ | GENERIC_WRITE,
                                                                      this->attributes(name), this->setup_iosb(), 3, options));
                return this->output().read();
            }

            handle create_event()
            {
                const auto event = this->emu->process.events.store(sogen::event{});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetEvent(this->context(), event.bits, {this->emu->memory}));
                return event;
            }

            bool is_signaled(const handle event)
            {
                const emulator_object<EVENT_BASIC_INFORMATION> information{this->emu->memory, this->storage + 0x500};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryEvent(this->context(), event, 0, information,
                                                                        sizeof(EVENT_BASIC_INFORMATION), {this->emu->memory}));
                return information.read().EventState != 0;
            }

            handle bind_completion_port(const handle file)
            {
                const auto port = this->emu->process.io_completions.store(io_completion{});
                EXPECT_EQ(STATUS_SUCCESS, this->emu->process.devices.get(file)->set_completion_association(
                                              this->emu->process, this->emu->vcpu(0).active_thread, port, completion_key));
                return port;
            }

            std::optional<io_completion_message> dequeue(const handle port)
            {
                io_completion_message message{};
                if (!io_completion_wait::dequeue_io_completion_message(*this->emu, port, message))
                {
                    return std::nullopt;
                }
                return message;
            }

            void work(const handle file)
            {
                this->emu->process.devices.get(file)->work(*this->emu);
            }

            void write_wait_request(const uint64_t address, const ULONG name_bytes_to_write)
            {
                this->emu->memory.write_memory(address + offsetof(file_pipe_wait_for_buffer, timeout), LARGE_INTEGER{.QuadPart = 0});
                this->emu->memory.write_memory(address + offsetof(file_pipe_wait_for_buffer, name_length), listening_name_bytes);
                this->emu->memory.write_memory(address + offsetof(file_pipe_wait_for_buffer, timeout_specified), BOOLEAN{FALSE});
                this->emu->memory.write_memory(address + wait_header, listening_name.data(), name_bytes_to_write);
            }

            NTSTATUS wait(const handle root, const buffer_case& input, const handle event = {}, const uint64_t apc_context = 0)
            {
                this->iosb().write({.Status = iosb_sentinel, .Information = 0xAAAAAAAA});
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtFsControlFile(this->context(), root, event, 0, apc_context, this->iosb(), FSCTL_PIPE_WAIT,
                                                            input.address, input.length, 0, 0);
                });
            }

            NTSTATUS write(const handle file, const buffer_case& data, const handle event = {})
            {
                this->iosb().write({.Status = iosb_sentinel, .Information = 0xAAAAAAAA});
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtWriteFile(this->context(), file, event.bits, 0, 0, this->iosb(), data.address, data.length,
                                                        {this->emu->memory}, {this->emu->memory});
                });
            }

            NTSTATUS read(const handle file)
            {
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtReadFile(this->context(), file, 0, 0, 0, this->read_iosb(), this->read_buffer(), 64,
                                                       {this->emu->memory}, {this->emu->memory});
                });
            }

            std::string read_data(const size_t size) const
            {
                std::string data(size, '\0');
                this->emu->memory.read_memory(this->read_buffer(), data.data(), data.size());
                return data;
            }

            buffer_case follow_up_data()
            {
                this->emu->memory.write_memory(this->storage + 0x2000, "OKOK", 4);
                return {.address = this->storage + 0x2000, .length = 4};
            }

            std::u16string unique_pipe_name()
            {
                std::u16string name = u"\\Device\\NamedPipe\\buffer-";
                for (const char digit : std::to_string(this->pipe_counter++))
                {
                    name.push_back(static_cast<char16_t>(digit));
                }
                return name;
            }

            void expect_write_reaches_no_peer(const pipe_writer& writer, const buffer_case& data, const bool read_pending)
            {
                const auto name = this->unique_pipe_name();
                const ULONG writer_options = writer.synchronous ? FILE_SYNCHRONOUS_IO_NONALERT : 0;
                const auto server = this->create_server(name, writer.server_writes ? writer_options : 0);
                const auto client = this->open(name, writer.server_writes ? 0 : writer_options);
                const auto writing_end = writer.server_writes ? server : client;
                const auto reading_end = writer.server_writes ? client : server;
                if (read_pending)
                {
                    ASSERT_EQ(STATUS_PENDING, this->read(reading_end));
                }

                const auto event = writer.synchronous ? handle{} : this->create_event();
                EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->write(writing_end, data, event));
                EXPECT_TRUE(this->iosb_untouched());
                if (event.bits)
                {
                    EXPECT_FALSE(this->is_signaled(event));
                }
                this->work(reading_end);
                if (read_pending)
                {
                    EXPECT_EQ(STATUS_PENDING, this->read_iosb().read().Status);
                }

                EXPECT_EQ(STATUS_SUCCESS, this->write(writing_end, this->follow_up_data()));
                if (read_pending)
                {
                    this->work(reading_end);
                }
                else
                {
                    EXPECT_EQ(STATUS_SUCCESS, this->read(reading_end));
                }
                EXPECT_EQ(STATUS_SUCCESS, this->read_iosb().read().Status);
                EXPECT_EQ(4u, this->read_iosb().read().Information);
                EXPECT_EQ("OKOK", this->read_data(4));
            }

            bool iosb_untouched() const
            {
                return this->iosb().read().Status == iosb_sentinel;
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            uint64_t edge{};
            unsigned pipe_counter{};
        };
    }

    TEST_F(NamedPipeRequestBufferTest, UnreadableWaitInputIsAnAccessViolation)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        const auto synchronous_root = this->open(u"\\Device\\NamedPipe\\", FILE_SYNCHRONOUS_IO_NONALERT);
        const auto asynchronous_root = this->open(u"\\Device\\NamedPipe\\");
        const auto port = this->bind_completion_port(asynchronous_root);

        const auto at_edge = this->edge - full_wait_request;
        this->write_wait_request(at_edge, listening_name_bytes);
        EXPECT_EQ(STATUS_SUCCESS, this->wait(synchronous_root, {.address = at_edge, .length = full_wait_request}));

        const auto name_split = this->edge - (wait_header + name_bytes_before_the_edge);
        const auto header_split = this->edge - sizeof(LARGE_INTEGER);
        const std::array cases{
            wait_case{.input = {.address = this->edge, .length = full_wait_request}},
            wait_case{.input = {.address = this->edge, .length = wait_header - 1}},
            wait_case{.input = {.address = header_split, .length = full_wait_request}},
            wait_case{.input = {.address = header_split, .length = wait_header - 1}},
            wait_case{.input = {.address = name_split, .length = full_wait_request}, .name_bytes_in_memory = name_bytes_before_the_edge},
            wait_case{.input = {.address = at_edge, .length = full_wait_request + 16}, .name_bytes_in_memory = listening_name_bytes},
        };
        for (const auto& [input, name_bytes_in_memory] : cases)
        {
            if (name_bytes_in_memory)
            {
                this->write_wait_request(input.address, *name_bytes_in_memory);
            }
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->wait(synchronous_root, input));
            EXPECT_TRUE(this->iosb_untouched());

            const auto event = this->create_event();
            EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->wait(asynchronous_root, input, event, completion_context));
            EXPECT_TRUE(this->iosb_untouched());
            EXPECT_FALSE(this->is_signaled(event));
            this->work(asynchronous_root);
            EXPECT_FALSE(this->dequeue(port).has_value());
        }
    }

    TEST_F(NamedPipeRequestBufferTest, WaitInputTheDriverRejectsIsAnInvalidParameter)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        const auto synchronous_root = this->open(u"\\Device\\NamedPipe\\", FILE_SYNCHRONOUS_IO_NONALERT);
        const auto asynchronous_root = this->open(u"\\Device\\NamedPipe\\");
        const auto port = this->bind_completion_port(asynchronous_root);

        const auto name_split = this->edge - (wait_header + name_bytes_before_the_edge);
        const std::array cases{
            buffer_case{.address = 0, .length = full_wait_request},
            buffer_case{.address = 0, .length = 0},
            buffer_case{.address = this->edge, .length = 0},
            buffer_case{.address = this->storage + 0x2000, .length = wait_header - 1},
            buffer_case{.address = name_split, .length = wait_header + name_bytes_before_the_edge},
        };
        for (const auto& input : cases)
        {
            this->write_wait_request(this->storage + 0x2000, listening_name_bytes);
            this->write_wait_request(name_split, name_bytes_before_the_edge);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->wait(synchronous_root, input));
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->iosb().read().Status);

            const auto event = this->create_event();
            ASSERT_EQ(STATUS_PENDING, this->wait(asynchronous_root, input, event, completion_context));
            this->work(asynchronous_root);
            EXPECT_EQ(STATUS_INVALID_PARAMETER, this->iosb().read().Status);
            EXPECT_TRUE(this->is_signaled(event));
            const auto packet = this->dequeue(port);
            ASSERT_TRUE(packet.has_value());
            EXPECT_EQ(STATUS_INVALID_PARAMETER, packet->io_status_block.Status);
            EXPECT_EQ(completion_context, packet->apc_context);
        }
    }

    TEST_F(NamedPipeRequestBufferTest, UnreadableWriteDataReachesNoPeer)
    {
        for (const auto& writer : all_writers)
        {
            for (const auto& data :
                 {buffer_case{.address = this->edge, .length = 100}, buffer_case{.address = this->edge - 64, .length = 128}})
            {
                for (const bool read_pending : {false, true})
                {
                    this->expect_write_reaches_no_peer(writer, data, read_pending);
                }
            }
        }
    }

    TEST_F(NamedPipeRequestBufferTest, HugeUnreadableWritesReachNoPeer)
    {
        for (const auto& writer : all_writers)
        {
            for (const ULONG length : {0x100000u, 0x10000000u, 0x7FFFFFFFu, 0x80000000u})
            {
                for (const bool read_pending : {false, true})
                {
                    this->expect_write_reaches_no_peer(writer, {.address = this->edge, .length = length}, read_pending);
                }
            }
            this->expect_write_reaches_no_peer(writer, {.address = this->edge, .length = 0xFFFFFFFF}, true);
            this->expect_write_reaches_no_peer(writer, {.address = this->edge - 0x1000, .length = 0x10000000}, false);
        }
    }

    TEST_F(NamedPipeRequestBufferTest, EmptyWriteAtAnUnreadableAddressSucceeds)
    {
        for (const bool synchronous : {true, false})
        {
            const auto name = this->unique_pipe_name();
            const auto server = this->create_server(name, synchronous ? FILE_SYNCHRONOUS_IO_NONALERT : 0);
            const auto client = this->open(name);
            ASSERT_EQ(STATUS_PENDING, this->read(client));
            EXPECT_EQ(STATUS_SUCCESS, this->write(server, {.address = this->edge, .length = 0}));
            EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
            EXPECT_EQ(0u, this->iosb().read().Information);
            this->work(client);
            EXPECT_EQ(STATUS_PENDING, this->read_iosb().read().Status);
        }
    }
}
