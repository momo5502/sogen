#include "emulation_test_utils.hpp"
#include <devices/named_pipe.hpp>
#include <emulator_utils.hpp>
#include <io_completion_wait.hpp>
#include <inherited_pipes.hpp>
#include <syscall_utils.hpp>
#include <chrono>
#include <gtest/gtest.h>

namespace sogen::syscalls
{
    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, handle thread_handle, uint32_t info_class,
                                             uint64_t thread_information, uint32_t thread_information_length,
                                             emulator_object<uint32_t> return_length);
    NTSTATUS handle_NtCancelIoFile(const syscall_context& c, handle file_handle,
                                   emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
    NTSTATUS handle_NtClose(const syscall_context& c, handle h);
    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, handle source_process_handle, handle source_handle,
                                      handle target_process_handle, emulator_object<handle> target_handle, ACCESS_MASK desired_access,
                                      ULONG handle_attributes, ULONG options);
    NTSTATUS handle_NtSetInformationObject(const syscall_context& c, handle object, OBJECT_INFORMATION_CLASS information_class,
                                           emulator_pointer information, ULONG length);
    NTSTATUS handle_NtQueryObject(const syscall_context& c, handle object, OBJECT_INFORMATION_CLASS information_class,
                                  emulator_pointer information, ULONG length, emulator_object<ULONG> return_length);
    NTSTATUS handle_NtCreateUserProcess(const syscall_context& c, emulator_object<handle> process_handle,
                                        emulator_object<handle> thread_handle, ACCESS_MASK process_desired_access,
                                        ACCESS_MASK thread_desired_access,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> process_object_attributes,
                                        emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> thread_object_attributes,
                                        ULONG process_flags, ULONG thread_flags,
                                        emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_parameters,
                                        emulator_object<PS_CREATE_INFO<EmulatorTraits<Emu64>>> create_info,
                                        emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list);
    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, handle h, BOOLEAN alertable, emulator_object<LARGE_INTEGER> timeout);
    NTSTATUS handle_NtOpenFile(const syscall_context& c, emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                               emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                               ULONG open_options);
}

namespace sogen::test
{
    TEST(NamedPipeStatusTest, ListeningAndConnectedAreNativeErrorStatuses)
    {
        EXPECT_EQ(0xC00000B3u, STATUS_PIPE_LISTENING);
        EXPECT_EQ(0xC00000B2u, STATUS_PIPE_CONNECTED);
        EXPECT_FALSE(NT_SUCCESS(STATUS_PIPE_LISTENING));
        EXPECT_FALSE(NT_SUCCESS(STATUS_PIPE_CONNECTED));
    }

    namespace
    {
        class capturing_process_manager final : public process_manager
        {
          public:
            std::optional<process_create_request> request{};

            process_create_result create_process(process_create_request creation) override
            {
                this->request = std::move(creation);
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
                return {.error = process_error::unavailable};
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

        class NamedPipeTest : public testing::Test
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
                container.granted_access = access;
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
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return syscalls::handle_NtClose(context, h);
            }

            bool io_pending(handle thread)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                EXPECT_EQ(STATUS_SUCCESS,
                          syscalls::handle_NtQueryInformationThread(context, thread, ThreadIsIoPending, this->storage + 0x48000,
                                                                    sizeof(ULONG), {this->emu->memory}));
                return this->emu->memory.read_memory<ULONG>(this->storage + 0x48000) != 0;
            }

            handle duplicate(handle h, ACCESS_MASK access = FILE_READ_DATA, ULONG attributes = 0)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                const emulator_object<handle> output{this->emu->memory, this->storage + 0x40000};
                EXPECT_EQ(syscalls::handle_NtDuplicateObject(context, CURRENT_PROCESS, h, CURRENT_PROCESS, output, access, attributes, 0),
                          STATUS_SUCCESS);
                return output.read();
            }

            void create_child(ULONG flags, std::optional<std::vector<handle>> inherited = {}, bool expect_manager_request = true)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                emulator_allocator allocator{this->emu->memory, this->storage + 0x30000, 0x10000};
                const auto name = allocator.make_unicode_string(u"C:\\test.exe");
                const auto parameters = allocator.reserve<RTL_USER_PROCESS_PARAMETERS64>();
                RTL_USER_PROCESS_PARAMETERS64 values{};
                values.ImagePathName = name.read();
                values.CommandLine = name.read();
                values.StandardInput = this->reader.bits;
                values.StandardOutput = this->writer.bits;
                values.StandardError = this->writer.bits;
                parameters.write(values);
                const auto creation = allocator.reserve<PS_CREATE_INFO<EmulatorTraits<Emu64>>>();
                PS_CREATE_INFO<EmulatorTraits<Emu64>> information{};
                information.Size = sizeof(information);
                creation.write(information);
                const auto process = allocator.reserve<handle>();
                const auto thread = allocator.reserve<handle>();
                emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attributes{this->emu->memory};
                if (inherited)
                {
                    const auto handles = allocator.reserve<handle>(std::max<size_t>(inherited->size(), 1));
                    for (size_t i = 0; i < inherited->size(); ++i)
                    {
                        handles.write(inherited->at(i), i);
                    }
                    attributes = allocator.reserve<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>>();
                    PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>> list{};
                    list.TotalLength = sizeof(list);
                    list.Attributes[0].Attribute = PsAttributeHandleList;
                    list.Attributes[0].Size = inherited->size() * sizeof(handle);
                    list.Attributes[0].ValuePtr = handles.value();
                    attributes.write(list);
                }
                EXPECT_EQ(syscalls::handle_NtCreateUserProcess(context, process, thread, 0, 0, {this->emu->memory}, {this->emu->memory},
                                                               flags, 0, parameters, creation, attributes),
                          STATUS_NOT_SUPPORTED);
                EXPECT_EQ(this->manager.request.has_value(), expect_manager_request);
            }

            std::string read_output(size_t size)
            {
                std::string data(size, '\0');
                this->emu->memory.read_memory(this->storage + 0x1000, data.data(), data.size());
                return data;
            }

            capturing_process_manager manager{};
            std::unique_ptr<windows_emulator> emu;
            handle reader{};
            handle writer{};
            uint64_t storage{};
        };

        class InheritedPipeTest : public NamedPipeTest
        {
          protected:
            void SetUp() override
            {
#ifdef __EMSCRIPTEN__
                GTEST_SKIP() << "Native TCP pipe transfer is unavailable in WebAssembly";
#else
                NamedPipeTest::SetUp();
#endif
            }
        };
    }

    TEST_F(NamedPipeTest, ListenRemainsPendingUntilClientConnects)
    {
        auto& server = this->pipe(this->reader);
        server.unopened_peer = this->pipe(this->writer).endpoint;
        const auto event_handle = this->emu->process.events.store(event{});
        const auto completion = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->emu->process.devices.get(this->reader)
                                      ->set_completion_association(this->emu->process, this->emu->vcpu(0).active_thread, completion, 42));
        auto context = this->request(this->reader, 0);
        context.io_control_code = FSCTL_PIPE_LISTEN;
        context.event = event_handle;
        context.apc_context = 123;
        EXPECT_EQ(STATUS_PENDING, this->emu->process.devices.get(this->reader)->execute_ioctl(*this->emu, context));
        server.work(*this->emu);
        EXPECT_EQ(STATUS_PENDING, context.io_status_block.read().Status);
        EXPECT_FALSE(this->emu->process.events.get(event_handle)->is_signaled());
        io_completion_message message{};
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
        auto client = std::move(server.unopened_peer);
        server.work(*this->emu);
        EXPECT_EQ(STATUS_SUCCESS, context.io_status_block.read().Status);
        EXPECT_TRUE(this->emu->process.events.get(event_handle)->is_signaled());
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
        EXPECT_EQ(42u, message.key_context);
        EXPECT_EQ(123u, message.apc_context);
        server.work(*this->emu);
        EXPECT_FALSE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
        EXPECT_EQ(STATUS_PIPE_CONNECTED, server.io_control(*this->emu, context));
    }

    TEST_F(NamedPipeTest, CancellingPendingListenSignalsCompletion)
    {
        auto& server = this->pipe(this->reader);
        server.unopened_peer = this->pipe(this->writer).endpoint;
        auto context = this->request(this->reader, 0);
        context.io_control_code = FSCTL_PIPE_LISTEN;
        const auto event_handle = this->emu->process.events.store(event{});
        context.event = event_handle;
        ASSERT_EQ(STATUS_PENDING, server.io_control(*this->emu, context));
        EXPECT_TRUE(server.cancel_io(*this->emu, context.io_status_block.value()));
        EXPECT_EQ(STATUS_CANCELLED, context.io_status_block.read().Status);
        EXPECT_TRUE(this->emu->process.events.get(event_handle)->is_signaled());
        server.work(*this->emu);
        EXPECT_EQ(STATUS_CANCELLED, context.io_status_block.read().Status);
    }

    TEST_F(NamedPipeTest, NonblockingListenReturnsNativeListeningStatus)
    {
        auto& server = this->pipe(this->reader);
        server.unopened_peer = this->pipe(this->writer).endpoint;
        server.completion_mode = 1;
        auto context = this->request(this->reader, 0);
        context.io_control_code = FSCTL_PIPE_LISTEN;
        EXPECT_EQ(STATUS_PIPE_LISTENING, server.io_control(*this->emu, context));
        EXPECT_FALSE(server.cancel_io(*this->emu, 0));
    }

    TEST_F(NamedPipeTest, SerializationPreservesPendingListen)
    {
        this->pipe(this->reader).unopened_peer = this->pipe(this->writer).endpoint;
        auto context = this->request(this->reader, 0);
        context.io_control_code = FSCTL_PIPE_LISTEN;
        ASSERT_EQ(STATUS_PENDING, this->pipe(this->reader).io_control(*this->emu, context));
        utils::buffer_serializer snapshot;
        this->emu->process.devices.get(this->reader)->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        input.register_factory<guest_machine_wrapper>([&] { return guest_machine_wrapper{this->emu->emu()}; });
        this->emu->process.devices.get(this->reader)->deserialize(input);
        auto& restored = this->pipe(this->reader);
        restored.work(*this->emu);
        EXPECT_EQ(STATUS_PENDING, context.io_status_block.read().Status);
        auto client = std::move(restored.unopened_peer);
        restored.work(*this->emu);
        EXPECT_EQ(STATUS_SUCCESS, context.io_status_block.read().Status);
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
        EXPECT_FALSE(this->emu->process.events.get(event)->is_signaled());
        ASSERT_EQ(this->write("hello pipe"), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(context.io_status_block.read().Information, 10u);
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
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

    TEST_F(NamedPipeTest, PendingIoQueryTracksReadCompletionAndCancellation)
    {
        const auto first = this->emu->process.threads.find_handle(this->emu->vcpu(0).active_thread);
        const auto second =
            this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 12, false});
        auto request = this->request(this->reader, 1);
        ASSERT_EQ(STATUS_PENDING, this->pipe(this->reader).start_io(*this->emu, request, true));
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(second);
        EXPECT_TRUE(this->io_pending(first));
        EXPECT_FALSE(this->io_pending(CURRENT_THREAD));
        ASSERT_EQ(STATUS_SUCCESS, this->write("x"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            this->pipe(this->reader).work(*this->emu);
        } while (request.io_status_block.read().Status == STATUS_PENDING && std::chrono::steady_clock::now() < deadline);
        ASSERT_EQ(STATUS_SUCCESS, request.io_status_block.read().Status);
        EXPECT_FALSE(this->io_pending(first));
        request = this->request(this->reader, 1);
        ASSERT_EQ(STATUS_PENDING, this->pipe(this->reader).start_io(*this->emu, request, true));
        EXPECT_TRUE(this->io_pending(CURRENT_THREAD));
        ASSERT_TRUE(this->pipe(this->reader).cancel_io(*this->emu, request.io_status_block.value()));
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do
        {
            this->pipe(this->reader).work(*this->emu);
        } while (request.io_status_block.read().Status == STATUS_PENDING && std::chrono::steady_clock::now() < cancel_deadline);
        EXPECT_EQ(STATUS_CANCELLED, request.io_status_block.read().Status);
        EXPECT_FALSE(this->io_pending(CURRENT_THREAD));
    }

    TEST_F(NamedPipeTest, LegacyCancellationLeavesOtherThreadsReadsPending)
    {
        const auto first = this->emu->process.threads.find_handle(this->emu->vcpu(0).active_thread);
        const auto second =
            this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 12, false});
        const auto read = [this](handle thread, uint64_t offset) {
            this->emu->vcpu(0).active_thread = this->emu->process.threads.get(thread);
            auto request = this->request(this->reader, 1);
            request.io_status_block.set_address(this->storage + offset);
            EXPECT_EQ(STATUS_PENDING, this->pipe(this->reader).start_io(*this->emu, request, true));
            return request.io_status_block;
        };
        const auto first_read = read(first, 0x600);
        const auto second_read = read(second, 0x620);
        const auto third_read = read(first, 0x640);
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelIoFile(context, this->reader, {this->emu->memory, this->storage + 0x700}));
        EXPECT_EQ(STATUS_CANCELLED, first_read.read().Status);
        EXPECT_EQ(STATUS_PENDING, second_read.read().Status);
        EXPECT_EQ(STATUS_CANCELLED, third_read.read().Status);
        this->emu->vcpu(0).active_thread = this->emu->process.threads.get(second);
        ASSERT_EQ(STATUS_SUCCESS, syscalls::handle_NtCancelIoFile(context, this->reader, {this->emu->memory, this->storage + 0x700}));
        EXPECT_EQ(STATUS_CANCELLED, second_read.read().Status);
    }

    TEST_F(NamedPipeTest, CancelledReadSignalsItsEvent)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        auto context = this->request(this->reader, 16);
        context.event = event;
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_PENDING);
        ASSERT_TRUE(this->pipe(this->reader).cancel_io(*this->emu, context.io_status_block.value()));
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_CANCELLED);
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
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
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
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
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        for (const std::u16string_view alias : {u"\\??\\PIPE\\", u"\\Device\\NamedPipe\\", u"\\??\\pipe\\"})
        {
            emulator_allocator allocator{this->emu->memory, this->storage + 0x30000, 0x10000};
            const auto name = allocator.make_unicode_string(alias);
            const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
            OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> object_attributes{};
            object_attributes.Length = sizeof(object_attributes);
            object_attributes.ObjectName = name.value();
            attributes.write(object_attributes);
            const auto output = allocator.reserve<handle>();
            const auto iosb = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
            ASSERT_EQ(syscalls::handle_NtOpenFile(syscall, output, GENERIC_READ, attributes, iosb, 3, 0), STATUS_SUCCESS);
            EXPECT_EQ(this->pipe(output.read()).name, u"\\Device\\NamedPipe\\");
            EXPECT_FALSE(this->pipe(output.read()).endpoint);
            EXPECT_EQ(this->close(output.read()), STATUS_SUCCESS);
        }
    }

    TEST_F(NamedPipeTest, DuplicateSharesSignalingAndCompletionAssociation)
    {
        const auto duplicate = this->duplicate(this->reader);
        ASSERT_NE(duplicate, this->reader);
        EXPECT_EQ(&this->pipe(duplicate), &this->pipe(this->reader));
        const auto completion = this->emu->process.io_completions.store(io_completion{});
        auto* original = this->emu->process.devices.get(this->reader);
        ASSERT_EQ(original->set_completion_association(this->emu->process, this->emu->vcpu(0).active_thread, completion, 42),
                  STATUS_SUCCESS);
        original->set_completion_notification_flags(1);
        auto context = this->request(duplicate, 16);
        EXPECT_EQ(this->emu->process.io_completions.get(context.completion_port), this->emu->process.io_completions.get(completion));
        EXPECT_EQ(context.completion_key, 42u);
        EXPECT_EQ(context.completion_notification_flags, 1u);
        ASSERT_EQ(this->pipe(duplicate).start_io(*this->emu, context, true), STATUS_PENDING);
        EXPECT_FALSE(this->pipe(this->reader).signaled);
        ASSERT_EQ(this->close(this->reader), STATUS_SUCCESS);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_PENDING);
        ASSERT_EQ(this->write("shared"), STATUS_SUCCESS);
        this->pipe(duplicate).work(*this->emu);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_SUCCESS);
        EXPECT_TRUE(this->pipe(duplicate).signaled);
        io_completion_message message{};
        ASSERT_TRUE(io_completion_wait::dequeue_io_completion_message(*this->emu, completion, message));
        EXPECT_EQ(message.key_context, 42u);
        EXPECT_EQ(this->close(duplicate), STATUS_SUCCESS);
        EXPECT_EQ(this->emu->process.io_completions.get(completion)->ref_count, 1u);
    }

    TEST_F(NamedPipeTest, DuplicateKeepsAccessAndHandleFlagsIndependent)
    {
        const auto duplicate = this->duplicate(this->reader, FILE_WRITE_DATA, OBJ_INHERIT);
        const auto* original = this->emu->process.devices.get(this->reader);
        auto* copy = this->emu->process.devices.get(duplicate);
        EXPECT_EQ(original->granted_access, FILE_READ_DATA);
        EXPECT_EQ(copy->granted_access, FILE_WRITE_DATA);
        EXPECT_FALSE(original->inherit);
        EXPECT_TRUE(copy->inherit);
        copy->protect_from_close = true;
        EXPECT_EQ(this->close(duplicate), STATUS_HANDLE_NOT_CLOSABLE);
        EXPECT_EQ(this->pipe(duplicate).start_io(*this->emu, this->request(duplicate, 1), true), STATUS_ACCESS_DENIED);
        EXPECT_EQ(this->close(this->reader), STATUS_SUCCESS);
        copy->protect_from_close = false;
        EXPECT_EQ(this->close(duplicate), STATUS_SUCCESS);
    }

    TEST_F(NamedPipeTest, DuplicateListeningServerAcceptsOnlyOneClient)
    {
        auto pair = utils::host_pipe::create_pair(false, 4096);
        const auto server = this->store_pipe(std::move(pair.first), FILE_READ_DATA);
        this->pipe(server).name = u"\\Device\\NamedPipe\\duplicate-listener";
        this->pipe(server).unopened_peer = std::move(pair.second);
        const auto duplicate = this->duplicate(server);
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        emulator_allocator allocator{this->emu->memory, this->storage + 0x30000, 0x10000};
        const auto name = allocator.make_unicode_string(this->pipe(server).name);
        const auto attributes = allocator.reserve<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>();
        OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>> object_attributes{};
        object_attributes.Length = sizeof(object_attributes);
        object_attributes.ObjectName = name.value();
        attributes.write(object_attributes);
        const auto output = allocator.reserve<handle>();
        const auto iosb = allocator.reserve<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>();
        ASSERT_EQ(syscalls::handle_NtOpenFile(context, output, FILE_WRITE_DATA, attributes, iosb, 3, 0), STATUS_SUCCESS);
        EXPECT_FALSE(this->pipe(server).unopened_peer);
        EXPECT_FALSE(this->pipe(duplicate).unopened_peer);
        EXPECT_FALSE(NT_SUCCESS(syscalls::handle_NtOpenFile(context, output, FILE_WRITE_DATA, attributes, iosb, 3, 0)));
    }

    TEST_F(NamedPipeTest, ClosingNeverConnectedPipeDoesNotThrow)
    {
        io_device_container container{u"NamedPipe", *this->emu, {}};
        const auto unopened = this->emu->process.devices.store(std::move(container));
        EXPECT_EQ(this->close(unopened), STATUS_SUCCESS);
    }

    TEST_F(NamedPipeTest, InvalidCompletionHandlesDoNotConsumeBufferedData)
    {
        ASSERT_EQ(this->write("untouched"), STATUS_SUCCESS);
        auto context = this->request(this->reader, 16);
        context.event = make_handle(0x7fffffff, handle_types::event, false);
        EXPECT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_INVALID_HANDLE);
        EXPECT_TRUE(this->pipe(this->reader).signaled);
        context = this->request(this->reader, 16);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, context, true), STATUS_SUCCESS);
        EXPECT_EQ(this->read_output(9), "untouched");
    }

    TEST_F(NamedPipeTest, InvalidCompletionHandlesDoNotPerformImmediateOrPartialWrites)
    {
        for (const uint32_t size : {1u, 8192u})
        {
            auto context = this->request(this->writer, size);
            context.event = this->emu->process.events.store(sogen::event{});
            context.completion_port = make_handle(0x7fffffff, handle_types::io_completion, false);
            const auto references = this->emu->process.events.get(context.event)->ref_count;
            EXPECT_EQ(this->pipe(this->writer).start_io(*this->emu, context, false), STATUS_INVALID_HANDLE);
            EXPECT_EQ(this->emu->process.events.get(context.event)->ref_count, references);
            EXPECT_TRUE(this->pipe(this->writer).signaled);
            std::string data;
            uint32_t available{};
            EXPECT_EQ(this->pipe(this->reader).endpoint->peek(data, size, available).status, utils::pipe_io_status::success);
            EXPECT_EQ(available, 0u);
        }
    }

    TEST_F(NamedPipeTest, SnapshotPreservesUnconnectedDuplicateFileIdentity)
    {
        io_device_container original{u"NamedPipe", *this->emu, {}};
        auto duplicate = original.duplicate(FILE_WRITE_DATA, true);
        utils::buffer_serializer buffer;
        original.serialize(buffer);
        duplicate.serialize(buffer);
        io_device_container restored_original;
        io_device_container restored_duplicate;
        utils::buffer_deserializer input{buffer};
        restored_original.deserialize(input);
        restored_duplicate.deserialize(input);
        EXPECT_EQ(restored_original.get_internal_device(), restored_duplicate.get_internal_device());
        EXPECT_EQ(restored_duplicate.granted_access, FILE_WRITE_DATA);
        EXPECT_TRUE(restored_duplicate.inherit);
        EXPECT_FALSE(restored_original.is_last_file_reference());
        utils::buffer_serializer roundtrip;
        restored_original.serialize(roundtrip);
        restored_duplicate.serialize(roundtrip);
        EXPECT_EQ(buffer.get_buffer(), roundtrip.get_buffer());
        restored_duplicate = {};
        EXPECT_TRUE(restored_original.is_last_file_reference());
    }

    TEST_F(InheritedPipeTest, InheritedAliasesKeepSharedFileIdentityAndCancellation)
    {
        const auto alias = this->duplicate(this->reader, FILE_WRITE_DATA, OBJ_INHERIT);
        this->emu->process.devices.get(this->reader)->set_completion_notification_flags(1);
        this->create_child(4, std::vector{this->reader, alias});
        ASSERT_EQ(this->manager.request->inherited_pipes.size(), 2u);
        const auto& pipes = this->manager.request->inherited_pipes;
        EXPECT_EQ(pipes.front().file_id, pipes.back().file_id);
        EXPECT_EQ(pipes.front().completion_notification_flags, 1u);
        ASSERT_EQ(this->close(this->reader), STATUS_SUCCESS);
        ASSERT_EQ(this->close(alias), STATUS_SUCCESS);
        this->emu->process.process_params64.set_address(this->storage + 0x30000);
        this->emu->process.process_params64.write({});
        this->emu->process.process_params32.emplace(this->emu->memory, this->storage + 0x32000);
        this->emu->process.process_params32->write({});
        install_inherited_pipes(*this->emu, *this->manager.request, std::move(this->manager.request->inherited_pipes));
        EXPECT_EQ(&this->pipe(this->reader), &this->pipe(alias));
        EXPECT_EQ(this->emu->process.devices.get(alias)->granted_access, FILE_WRITE_DATA);
        EXPECT_TRUE(this->emu->process.devices.get(alias)->inherit);
        EXPECT_EQ(this->emu->process.process_params64.read().StandardInput, this->reader.bits);
        EXPECT_EQ(this->emu->process.process_params32->read().StandardInput, static_cast<uint32_t>(this->reader.bits));
        EXPECT_EQ(this->request(alias, 4).completion_notification_flags, 1u);
        const auto pending = this->request(this->reader, 4);
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, pending, true), STATUS_PENDING);
        EXPECT_FALSE(this->pipe(alias).signaled);
        EXPECT_TRUE(this->emu->process.devices.get(alias)->cancel_io(*this->emu, pending.io_status_block.value()));
        EXPECT_TRUE(this->pipe(this->reader).signaled);
        EXPECT_EQ(pending.io_status_block.read().Status, STATUS_CANCELLED);
    }

    TEST_F(NamedPipeTest, ListeningLocalPipeSnapshotRestoresPeerAndConfiguration)
    {
        io_device_container original{u"NamedPipe", *this->emu, {}};
        auto* pipe = original.get_internal_device<named_pipe>();
        auto pair = utils::host_pipe::create_pair(false, 4);
        pipe->endpoint = pair.first;
        pipe->unopened_peer = pair.second;
        pipe->name = u"snapshot";
        pipe->max_instances = 1;
        pipe->inbound_quota = 4;
        pipe->outbound_quota = 4;
        pipe->default_timeout.QuadPart = -1000;
        utils::buffer_serializer snapshot;
        original.serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        io_device_container restored;
        restored.deserialize(input);
        auto* restored_pipe = restored.get_internal_device<named_pipe>();
        ASSERT_NE(restored_pipe->endpoint, nullptr);
        ASSERT_NE(restored_pipe->unopened_peer, nullptr);
        EXPECT_EQ(restored_pipe->name, u"snapshot");
        EXPECT_EQ(restored_pipe->max_instances, 1u);
        EXPECT_EQ(restored_pipe->default_timeout.QuadPart, -1000);
        auto write = restored_pipe->unopened_peer->write("hi");
        EXPECT_EQ(write->check_completion().status, utils::pipe_io_status::success);
        auto read = restored_pipe->endpoint->read(4);
        EXPECT_EQ(read->check_completion().transferred, 2u);
        EXPECT_EQ(read->data(), "hi");
    }

    TEST_F(NamedPipeTest, LocalPipeSnapshotRestoresPendingIoAndCompletionReferences)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        auto pending = this->request(this->reader, 4);
        pending.event = event;
        ASSERT_EQ(this->pipe(this->reader).start_io(*this->emu, pending, true), STATUS_PENDING);
        utils::buffer_serializer snapshot;
        this->emu->process.devices.get(this->reader)->serialize(snapshot);
        this->emu->process.devices.get(this->writer)->serialize(snapshot);
        utils::buffer_deserializer input{snapshot};
        input.register_factory<guest_machine_wrapper>([&] { return guest_machine_wrapper{this->emu->emu()}; });
        this->emu->process.devices.get(this->reader)->deserialize(input);
        this->emu->process.devices.get(this->writer)->deserialize(input);
        EXPECT_FALSE(this->pipe(this->reader).signaled);
        ASSERT_EQ(this->write("ok"), STATUS_SUCCESS);
        this->pipe(this->reader).work(*this->emu);
        EXPECT_EQ(pending.io_status_block.read().Status, STATUS_SUCCESS);
        EXPECT_EQ(this->read_output(2), "ok");
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
        EXPECT_EQ(this->emu->process.events.get(event)->ref_count, 1u);
    }

    TEST_F(NamedPipeTest, StdioDoesNotBypassDisabledInheritance)
    {
        this->emu->process.devices.get(this->reader)->inherit = true;
        this->emu->process.devices.get(this->writer)->inherit = true;
        this->create_child(0);
        EXPECT_TRUE(this->manager.request->inherited_pipes.empty());
        EXPECT_EQ(this->manager.request->standard_input, 0u);
        EXPECT_EQ(this->manager.request->standard_output, 0u);
        EXPECT_EQ(this->manager.request->standard_error, 0u);
    }

    TEST_F(InheritedPipeTest, GeneralInheritanceSelectsOnlyInheritableHandles)
    {
        this->emu->process.devices.get(this->writer)->inherit = true;
        this->create_child(4);
        ASSERT_EQ(this->manager.request->inherited_pipes.size(), 1u);
        EXPECT_EQ(this->manager.request->inherited_pipes.front().guest_handle, this->writer.bits);
        EXPECT_EQ(this->manager.request->standard_input, 0u);
        EXPECT_EQ(this->manager.request->standard_output, this->writer.bits);
        EXPECT_EQ(this->manager.request->standard_error, this->writer.bits);
    }

    TEST_F(InheritedPipeTest, ExplicitHandleListExcludesOtherStdio)
    {
        this->emu->process.devices.get(this->reader)->inherit = true;
        this->emu->process.devices.get(this->writer)->inherit = true;
        this->create_child(4, std::vector{this->reader});
        ASSERT_EQ(this->manager.request->inherited_pipes.size(), 1u);
        EXPECT_EQ(this->manager.request->inherited_pipes.front().guest_handle, this->reader.bits);
        EXPECT_EQ(this->manager.request->standard_input, this->reader.bits);
        EXPECT_EQ(this->manager.request->standard_output, 0u);
        EXPECT_EQ(this->manager.request->standard_error, 0u);
    }

    TEST_F(NamedPipeTest, ExplicitHandleListCannotEnableInheritance)
    {
        this->emu->process.devices.get(this->reader)->inherit = true;
        this->create_child(0, std::vector{this->reader});
        EXPECT_TRUE(this->manager.request->inherited_pipes.empty());
        EXPECT_EQ(this->manager.request->standard_input, 0u);
    }

    TEST_F(NamedPipeTest, EmptyExplicitHandleListDoesNotInheritStdio)
    {
        this->emu->process.devices.get(this->writer)->inherit = true;
        this->create_child(4, std::vector<handle>{});
        EXPECT_TRUE(this->manager.request->inherited_pipes.empty());
        EXPECT_EQ(this->manager.request->standard_output, 0u);
    }

    TEST_F(NamedPipeTest, MessageReadModeIsRejectedRatherThanTreatedAsBytes)
    {
        this->pipe(this->reader).pipe_type = 1;
        this->pipe(this->reader).read_mode = 1;
        this->emu->process.devices.get(this->reader)->inherit = true;
        EXPECT_EQ(this->pipe(this->reader).start_io(*this->emu, this->request(this->reader, 16), true), STATUS_NOT_SUPPORTED);
        this->create_child(4, {}, false);
    }

    TEST_F(NamedPipeTest, HandleFlagsAlsoApplyToNonPipeDevices)
    {
        io_device_container container{u"DeviceApi\\CMApi", *this->emu, {}};
        const auto device = this->emu->process.devices.store(std::move(container));
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        const emulator_object<OBJECT_HANDLE_FLAG_INFORMATION> flags{this->emu->memory, this->storage + 0x40000};
        OBJECT_HANDLE_FLAG_INFORMATION values{};
        values.Inherit = TRUE;
        values.ProtectFromClose = TRUE;
        flags.write(values);
        ASSERT_EQ(syscalls::handle_NtSetInformationObject(context, device, ObjectHandleFlagInformation, flags.value(), sizeof(values)),
                  STATUS_SUCCESS);
        flags.write({});
        ASSERT_EQ(syscalls::handle_NtQueryObject(context, device, ObjectHandleFlagInformation, flags.value(), sizeof(values),
                                                 {this->emu->memory}),
                  STATUS_SUCCESS);
        EXPECT_TRUE(flags.read().Inherit);
        EXPECT_TRUE(flags.read().ProtectFromClose);
        EXPECT_EQ(this->close(device), STATUS_HANDLE_NOT_CLOSABLE);
        flags.write({});
        ASSERT_EQ(syscalls::handle_NtSetInformationObject(context, device, ObjectHandleFlagInformation, flags.value(), sizeof(values)),
                  STATUS_SUCCESS);
        EXPECT_EQ(this->close(device), STATUS_SUCCESS);
    }

    TEST_F(NamedPipeTest, WrongObjectTypeCannotBeUsedAsCompletionEvent)
    {
        auto context = this->request(this->writer, 1);
        context.event = this->reader;
        EXPECT_EQ(this->pipe(this->writer).start_io(*this->emu, context, false), STATUS_INVALID_HANDLE);
        EXPECT_TRUE(this->pipe(this->writer).signaled);
    }
}
