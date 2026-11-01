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
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtQueryEvent(const syscall_context& c, handle event_handle, uint32_t event_information_class,
                                 emulator_object<EVENT_BASIC_INFORMATION> event_information, uint32_t event_information_length,
                                 emulator_object<uint32_t> return_length);
}

namespace sogen::test
{
    namespace
    {
        constexpr LONGLONG milliseconds_300 = -300LL * 10000;
        constexpr LONGLONG milliseconds_400 = -400LL * 10000;

        class pipe_wait_test_clock : public utils::clock
        {
          public:
            steady_time_point steady_now() override
            {
                return steady_time_point{} + this->elapsed;
            }

            std::chrono::nanoseconds elapsed{};
        };

        struct wait_request
        {
            std::u16string_view name;
            LONGLONG timeout{milliseconds_300};
            bool timeout_specified{true};
            std::optional<ULONG> name_length{};
            std::optional<uint64_t> iosb_address{};
            uint64_t apc_context{};
        };

        constexpr uint64_t unmapped_iosb = 0x10;
        constexpr uint64_t completion_key = 0x77;
        constexpr uint64_t completion_context = 0x5678;

        class NamedPipeWaitTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                auto test_clock = std::make_unique<pipe_wait_test_clock>();
                this->clock = test_clock.get();
                interfaces.clock = std::move(test_clock);
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
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> attributes(const std::u16string_view name, const handle root = {})
            {
                emulator_allocator allocator{this->emu->memory, this->storage + 0x1000, 0x1000};
                const auto string = allocator.make_unicode_string(name);
                const auto object = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
                OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> values{};
                values.Length = sizeof(values);
                values.ObjectName = string.value();
                values.RootDirectory = root.bits;
                object.write(values);
                return object;
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb() const
            {
                return {this->emu->memory, this->storage + 0x100};
            }

            emulator_object<handle> output() const
            {
                return {this->emu->memory, this->storage + 0x200};
            }

            emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> setup_iosb() const
            {
                return {this->emu->memory, this->storage + 0x180};
            }

            handle create_server(const std::u16string_view name, const ULONG maximum_instances = 1, const handle root = {})
            {
                const emulator_object<LARGE_INTEGER> default_timeout{this->emu->memory, this->storage + 0x300};
                default_timeout.write({.QuadPart = milliseconds_400});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtCreateNamedPipeFile(
                                              this->context(), this->output(), GENERIC_READ | GENERIC_WRITE, this->attributes(name, root),
                                              this->setup_iosb(), 3, 3, 0, 1, 1, 0, maximum_instances, 4096, 4096, default_timeout));
                return this->output().read();
            }

            NTSTATUS open(const std::u16string_view name, const ULONG options = 0, const handle root = {})
            {
                return syscalls::handle_NtOpenFile(this->context(), this->output(), GENERIC_READ | GENERIC_WRITE,
                                                   this->attributes(name, root), this->setup_iosb(), 3, options);
            }

            handle open_root(const ULONG options, const std::u16string_view path = u"\\Device\\NamedPipe\\")
            {
                EXPECT_EQ(STATUS_SUCCESS, this->open(path, options));
                return this->output().read();
            }

            NTSTATUS wait(const handle root, const wait_request& request, const handle event = {})
            {
                const auto name_bytes = static_cast<ULONG>(request.name.size() * sizeof(char16_t));
                const auto input = this->storage + 0x2000;
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, timeout),
                                               LARGE_INTEGER{.QuadPart = request.timeout});
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, name_length),
                                               request.name_length.value_or(name_bytes));
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, timeout_specified),
                                               static_cast<BOOLEAN>(request.timeout_specified));
                this->emu->memory.write_memory(input + offsetof(file_pipe_wait_for_buffer, name), request.name.data(), name_bytes);
                const auto length = static_cast<ULONG>(offsetof(file_pipe_wait_for_buffer, name)) + name_bytes;
                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> block{this->emu->memory,
                                                                                    request.iosb_address.value_or(this->iosb().value())};
                return this->wait_raw(root, input, length, event, block, request.apc_context);
            }

            NTSTATUS wait_raw(const handle root, const uint64_t input, const ULONG length, const handle event = {})
            {
                return this->wait_raw(root, input, length, event, this->iosb(), 0);
            }

            NTSTATUS wait_raw(const handle root, const uint64_t input, const ULONG length, const handle event,
                              const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> block, const uint64_t apc_context)
            {
                this->iosb().write({.Status = static_cast<NTSTATUS>(0xAAAAAAAA), .Information = 0xAAAAAAAA});
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtFsControlFile(this->context(), root, event, 0, apc_context, block, FSCTL_PIPE_WAIT, input,
                                                            length, 0, 0);
                });
            }

            bool iosb_untouched() const
            {
                return this->iosb().read().Status == static_cast<NTSTATUS>(0xAAAAAAAA);
            }

            handle closed_event()
            {
                const auto event = this->emu->process.events.store(sogen::event{});
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), event));
                return event;
            }

            void work(const handle file)
            {
                this->emu->process.devices.get(file)->work(*this->emu);
            }

            bool is_signaled(const handle event)
            {
                const emulator_object<EVENT_BASIC_INFORMATION> information{this->emu->memory, this->storage + 0x500};
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtQueryEvent(this->context(), event, 0, information,
                                                                        sizeof(EVENT_BASIC_INFORMATION), {this->emu->memory}));
                return information.read().EventState != 0;
            }

            std::chrono::milliseconds clock_now() const
            {
                return std::chrono::duration_cast<std::chrono::milliseconds>(this->clock->elapsed);
            }

            void advance_and_work(const handle root, const std::chrono::milliseconds elapsed)
            {
                this->clock->elapsed = elapsed;
                this->emu->process.devices.get(root)->work(*this->emu);
            }

            std::unique_ptr<windows_emulator> emu{};
            pipe_wait_test_clock* clock{};
            uint64_t storage{};
        };
    }

    TEST_F(NamedPipeWaitTest, MissingNameFailsWithoutWaiting)
    {
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->wait(root, {.name = u"missing"}));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->iosb().read().Status);
        EXPECT_EQ(0u, this->iosb().read().Information);
    }

    TEST_F(NamedPipeWaitTest, MalformedRequestsAreInvalidParameters)
    {
        this->create_server(u"\\Device\\NamedPipe\\malformed");
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->wait_raw(root, this->storage + 0x2000, 13));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->iosb().read().Status);
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->wait(root, {.name = u"malformed", .name_length = 0x1000}));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->wait(root, {.name = u""}));
    }

    TEST_F(NamedPipeWaitTest, ListeningInstanceSatisfiesWaitAtOnce)
    {
        const auto server = this->create_server(u"\\Device\\NamedPipe\\available");
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        EXPECT_EQ(STATUS_SUCCESS, this->wait(root, {.name = u"available"}));
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
        EXPECT_EQ(0u, this->iosb().read().Information);
        EXPECT_EQ(STATUS_SUCCESS, this->wait(root, {.name = u"\\available"}));

        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> listen_iosb{this->emu->memory, this->storage + 0x400};
        ASSERT_EQ(STATUS_PENDING,
                  syscalls::handle_NtFsControlFile(this->context(), server, {}, 0, 0, listen_iosb, FSCTL_PIPE_LISTEN, 0, 0, 0, 0));
        EXPECT_EQ(STATUS_SUCCESS, this->wait(root, {.name = u"available"}));
    }

    TEST_F(NamedPipeWaitTest, BusyPipeTimesOutAfterRequestedTimeout)
    {
        this->create_server(u"\\Device\\NamedPipe\\busy");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\busy"));
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"busy"}));
        this->advance_and_work(root, std::chrono::milliseconds(299));
        EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
        EXPECT_FALSE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
        this->advance_and_work(root, std::chrono::milliseconds(300));
        EXPECT_EQ(STATUS_IO_TIMEOUT, this->iosb().read().Status);
        EXPECT_EQ(0u, this->iosb().read().Information);
        EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
    }

    TEST_F(NamedPipeWaitTest, UnspecifiedTimeoutUsesThePipeDefault)
    {
        this->create_server(u"\\Device\\NamedPipe\\default");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\default"));
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"default", .timeout = 0, .timeout_specified = false}));
        this->advance_and_work(root, std::chrono::milliseconds(399));
        EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
        this->advance_and_work(root, std::chrono::milliseconds(400));
        EXPECT_EQ(STATUS_IO_TIMEOUT, this->iosb().read().Status);
    }

    TEST_F(NamedPipeWaitTest, AsynchronousHandleReportsTimeoutThroughEvent)
    {
        this->create_server(u"\\Device\\NamedPipe\\async");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\async"));
        const auto root = this->open_root(0);
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"async"}, event));
        EXPECT_FALSE(this->is_signaled(event));
        this->advance_and_work(root, std::chrono::milliseconds(300));
        EXPECT_TRUE(this->is_signaled(event));
        EXPECT_EQ(STATUS_IO_TIMEOUT, this->iosb().read().Status);
        EXPECT_EQ(0u, this->iosb().read().Information);
    }

    TEST_F(NamedPipeWaitTest, SnapshotPreservesPendingWait)
    {
        const auto server = this->create_server(u"\\Device\\NamedPipe\\snapshot");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\snapshot"));
        const auto root = this->open_root(0);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"snapshot"}));
        utils::buffer_serializer snapshot;
        this->emu->process.devices.get(server)->serialize(snapshot);
        this->emu->process.devices.get(root)->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        input.register_factory<guest_machine_wrapper>([&] { return guest_machine_wrapper{this->emu->emu()}; });
        this->emu->process.devices.get(server)->deserialize(input);
        this->emu->process.devices.get(root)->deserialize(input);
        this->advance_and_work(root, std::chrono::milliseconds(299));
        EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
        this->advance_and_work(root, std::chrono::milliseconds(300));
        EXPECT_EQ(STATUS_IO_TIMEOUT, this->iosb().read().Status);
    }

    TEST_F(NamedPipeWaitTest, ClientOpenDistinguishesBusyFromMissingPipe)
    {
        this->create_server(u"\\Device\\NamedPipe\\single");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\single"));
        EXPECT_EQ(STATUS_PIPE_NOT_AVAILABLE, this->open(u"\\Device\\NamedPipe\\single"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->open(u"\\Device\\NamedPipe\\missing"));
    }

    TEST_F(NamedPipeWaitTest, KernelbaseRootAliasReachesTheWaitHandler)
    {
        this->create_server(u"\\??\\pipe\\alias");
        for (const std::u16string_view path : {u"\\DosDevices\\pipe\\", u"\\DosDevices\\PIPE\\", u"\\??\\pipe\\"})
        {
            const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT, path);
            EXPECT_EQ(STATUS_SUCCESS, this->wait(root, {.name = u"alias"}));
            EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->wait(root, {.name = u"missing"}));
        }
        EXPECT_EQ(STATUS_SUCCESS, this->open(u"\\??\\pipe\\alias"));
    }

    TEST_F(NamedPipeWaitTest, QueuedWaitCompletesWhenAnInstanceListens)
    {
        constexpr ULONG unlimited_instances = 0xFFFFFFFF;
        this->create_server(u"\\Device\\NamedPipe\\next", unlimited_instances);
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\next"));
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"next", .timeout = -3000LL * 10000}));
        this->advance_and_work(root, std::chrono::milliseconds(300));
        EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
        this->create_server(u"\\Device\\NamedPipe\\next", unlimited_instances);
        this->advance_and_work(root, std::chrono::milliseconds(301));
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
        EXPECT_EQ(0u, this->iosb().read().Information);
        EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
    }

    TEST_F(NamedPipeWaitTest, RootOpenLeavesAnAnonymousPipeToItsClient)
    {
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        const auto server = this->create_server(u"", 1, root);
        this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        EXPECT_EQ(STATUS_SUCCESS, this->open(u"", FILE_SYNCHRONOUS_IO_NONALERT, server));
    }

    TEST_F(NamedPipeWaitTest, PendingWaitEndsWhenTheServerEndIsClosed)
    {
        for (const bool close_client : {true, false})
        {
            const auto name = close_client ? std::u16string_view{u"closed-both"} : std::u16string_view{u"closed-server"};
            const auto server = this->create_server(u"\\Device\\NamedPipe\\" + std::u16string(name));
            ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\" + std::u16string(name)));
            const auto client = this->output().read();
            const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
            ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = name, .timeout = -2000LL * 10000}));
            this->advance_and_work(root, std::chrono::milliseconds(300));
            ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), server));
            if (close_client)
            {
                ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), client));
            }
            this->advance_and_work(root, std::chrono::milliseconds(301));
            EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->iosb().read().Status);
            EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
        }
    }

    TEST_F(NamedPipeWaitTest, ClosingOnlyTheClientLeavesThePipeBusy)
    {
        this->create_server(u"\\Device\\NamedPipe\\closed-client");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\closed-client"));
        const auto client = this->output().read();
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"closed-client", .timeout = -2000LL * 10000}));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), client));
        this->advance_and_work(root, std::chrono::milliseconds(1999));
        EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
        this->advance_and_work(root, std::chrono::milliseconds(2000));
        EXPECT_EQ(STATUS_IO_TIMEOUT, this->iosb().read().Status);
    }

    TEST_F(NamedPipeWaitTest, CreatingAnInstanceCompletesTheWaitBeforeAClientTakesIt)
    {
        constexpr ULONG unlimited_instances = 0xFFFFFFFF;
        this->create_server(u"\\Device\\NamedPipe\\taken", unlimited_instances);
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\taken"));
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"taken", .timeout = -2000LL * 10000}));
        this->create_server(u"\\Device\\NamedPipe\\taken", unlimited_instances);
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\taken"));
        EXPECT_EQ(STATUS_SUCCESS, this->iosb().read().Status);
        EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
    }

    TEST_F(NamedPipeWaitTest, ClosingTheLastServerEndFailsTheWaitBeforeTheNameReturns)
    {
        const auto server = this->create_server(u"\\Device\\NamedPipe\\returns");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\returns"));
        const auto root = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"returns", .timeout = -2000LL * 10000}));
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtClose(this->context(), server));
        this->create_server(u"\\Device\\NamedPipe\\returns");
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->iosb().read().Status);
        EXPECT_TRUE(this->emu->vcpu(0).thread().is_thread_ready(*this->emu));
    }

    TEST_F(NamedPipeWaitTest, OnlyThePipeDirectoryServesWaits)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        this->create_server(u"\\Device\\NamedPipe\\busy");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\busy"));
        const auto server = this->create_server(u"\\Device\\NamedPipe\\target");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\target", FILE_SYNCHRONOUS_IO_NONALERT));
        const auto client = this->output().read();
        const auto no_slash = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT, u"\\Device\\NamedPipe");
        const auto no_slash_async = this->open_root(0, u"\\Device\\NamedPipe");
        for (const std::u16string_view name : {u"listening", u"missing", u"busy"})
        {
            for (const auto target : {client, no_slash})
            {
                EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->wait(target, {.name = name}));
                EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->iosb().read().Status);
            }
            for (const auto target : {server, no_slash_async})
            {
                ASSERT_EQ(STATUS_PENDING, this->wait(target, {.name = name}));
                this->work(target);
                EXPECT_EQ(STATUS_ILLEGAL_FUNCTION, this->iosb().read().Status);
            }
        }
    }

    TEST_F(NamedPipeWaitTest, AsynchronousDirectoryDeliversEveryResultThroughTheIosb)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        const auto root = this->open_root(0);
        const auto event = this->emu->process.events.store(sogen::event{});
        for (const auto& [name, status] : {std::pair{u"listening", STATUS_SUCCESS}, std::pair{u"missing", STATUS_OBJECT_NAME_NOT_FOUND}})
        {
            ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = name}, event));
            EXPECT_EQ(STATUS_PENDING, this->iosb().read().Status);
            this->work(root);
            EXPECT_EQ(status, this->iosb().read().Status);
            EXPECT_EQ(0u, this->iosb().read().Information);
            EXPECT_TRUE(this->is_signaled(event));
        }
    }

    TEST_F(NamedPipeWaitTest, CompletionPortGetsAPacketForEveryResult)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        this->create_server(u"\\Device\\NamedPipe\\busy");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\busy"));
        const auto root = this->open_root(0);
        const auto port = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->emu->process.devices.get(root)->set_completion_association(
                                      this->emu->process, this->emu->vcpu(0).active_thread, port, completion_key));
        const std::array cases{std::pair{u"listening", STATUS_SUCCESS}, std::pair{u"missing", STATUS_OBJECT_NAME_NOT_FOUND},
                               std::pair{u"busy", STATUS_IO_TIMEOUT}};
        for (const auto& [name, status] : cases)
        {
            ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = name, .timeout = -200LL * 10000, .apc_context = completion_context}));
            this->advance_and_work(root, this->clock_now() + std::chrono::milliseconds(200));
            io_completion_message message{};
            ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, port, message));
            EXPECT_EQ(status, message.io_status_block.Status);
            EXPECT_EQ(0u, message.io_status_block.Information);
            EXPECT_EQ(completion_key, message.key_context);
            EXPECT_EQ(completion_context, message.apc_context);
        }
        EXPECT_EQ(STATUS_INVALID_HANDLE, this->wait(root, {.name = u"listening", .apc_context = completion_context}, this->closed_event()));
        this->work(root);
        io_completion_message message{};
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, port, message));
    }

    TEST_F(NamedPipeWaitTest, RequestHandlesAreCheckedBeforeTheTarget)
    {
        this->create_server(u"\\Device\\NamedPipe\\listening");
        this->create_server(u"\\Device\\NamedPipe\\busy");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\busy"));
        const auto server = this->create_server(u"\\Device\\NamedPipe\\target");
        ASSERT_EQ(STATUS_SUCCESS, this->open(u"\\Device\\NamedPipe\\target", FILE_SYNCHRONOUS_IO_NONALERT));
        const auto client = this->output().read();
        const auto directory = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT);
        const auto directory_async = this->open_root(0);
        const auto no_slash = this->open_root(FILE_SYNCHRONOUS_IO_NONALERT, u"\\Device\\NamedPipe");
        for (const auto target : {server, client, directory, directory_async, no_slash})
        {
            for (const std::u16string_view name : {u"listening", u"missing", u"busy"})
            {
                EXPECT_EQ(STATUS_INVALID_HANDLE, this->wait(target, {.name = name}, this->closed_event()));
                EXPECT_TRUE(this->iosb_untouched());
                EXPECT_EQ(STATUS_OBJECT_TYPE_MISMATCH, this->wait(target, {.name = name}, directory));
                EXPECT_TRUE(this->iosb_untouched());
                EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->wait(target, {.name = name, .iosb_address = 0}));
                EXPECT_EQ(STATUS_ACCESS_VIOLATION, this->wait(target, {.name = name, .iosb_address = unmapped_iosb}));
            }
        }
    }

    TEST_F(NamedPipeWaitTest, SnapshotPreservesADecidedResult)
    {
        const auto root = this->open_root(0);
        ASSERT_EQ(STATUS_PENDING, this->wait(root, {.name = u"missing"}));
        utils::buffer_serializer snapshot;
        this->emu->process.devices.get(root)->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        input.register_factory<guest_machine_wrapper>([&] { return guest_machine_wrapper{this->emu->emu()}; });
        this->emu->process.devices.get(root)->deserialize(input);
        this->work(root);
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, this->iosb().read().Status);
    }
}
