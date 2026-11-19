#include "std_include.hpp"

#include <console_backends/buffered_console_backend.hpp>

#include <devices/console.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, handle file_handle,
                                         emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t file_information,
                                         ULONG length, FILE_INFORMATION_CLASS info_class);
    NTSTATUS handle_NtReadFile(const syscall_context& c, handle file_handle, uint64_t event, uint64_t apc_routine, uint64_t apc_context,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, uint64_t buffer, ULONG length,
                               emulator_object<LARGE_INTEGER> byte_offset, emulator_object<ULONG> key);
    NTSTATUS handle_NtCancelIoFileEx(const syscall_context& c, handle file_handle,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_request_to_cancel,
                                     emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block);
}

namespace sogen::test
{
    namespace
    {
        class queued_console_backend final : public buffered_console_backend
        {
          public:
            void queue(const std::string_view input)
            {
                this->input_.insert(this->input_.end(), input.begin(), input.end());
            }

            void close_input()
            {
                this->input_closed_ = true;
            }

            bool cancel_read_input() override
            {
                ++this->cancellations;
                return !this->cancellation_pending;
            }

            void set_input_mode(const console_input_mode& mode) override
            {
                mode_ = mode;
            }

            console_size get_size() const override
            {
                return this->size_;
            }

            console_input_mode mode_{};
            console_size size_{};
            size_t blocking_refills{};
            size_t cancellations{};
            bool cancellation_pending{};

          protected:
            void refill(const int timeout_ms) override
            {
                this->blocking_refills += timeout_ms != 0;
            }
        };

        class recording_console_backend final : public console_backend
        {
          public:
            bool input_available() override
            {
                return false;
            }

            std::optional<console_key_event> read_input_event(bool, bool) override
            {
                return std::nullopt;
            }

            std::string read_input(size_t) override
            {
                return {};
            }

            std::optional<std::string> try_read_input(size_t) override
            {
                return std::string{};
            }

            void set_input_mode(const console_input_mode& mode) override
            {
                this->mode_ = mode;
            }

            void reset() override
            {
                this->mode_ = {};
                ++this->reset_count_;
            }

            console_input_mode mode_{};
            size_t reset_count_{};
        };

        struct console_mode_request
        {
            uint64_t target_handle{};
            uint32_t input_count{};
            uint32_t output_count{};
            uint32_t message_buffer_size{};
            uint32_t padding{};
            uint64_t message{};
            uint64_t data_size{};
            uint64_t data{};
            uint32_t api_number{};
            uint32_t message_data_size{};
            uint32_t mode{};
        };

        struct console_font_request
        {
            uint8_t maximum_window{};
            std::array<uint8_t, 3> padding{};
            uint32_t index{};
            int16_t width{};
            int16_t height{};
            uint32_t family{};
            uint32_t weight{};
            std::array<char16_t, 32> face_name{};
        };

        static_assert(sizeof(console_font_request) == 84);

        struct console_buffer_info
        {
            std::array<int16_t, 2> size{};
            std::array<int16_t, 2> cursor{};
            std::array<int16_t, 2> window_origin{};
            uint16_t attributes{};
            std::array<int16_t, 2> window_size{};
            std::array<int16_t, 2> maximum_window_size{};
            uint16_t popup_attributes{};
            uint8_t fullscreen_supported{};
            std::array<uint32_t, 16> color_table{};
        };

        static_assert(sizeof(console_buffer_info) == 92);

        struct console_text_request
        {
            uint32_t bytes_written{};
            uint8_t unicode{};
            std::array<uint8_t, 3> padding{};
        };

        struct console_title_request
        {
            uint32_t length{};
            uint8_t unicode{};
            uint8_t original{};
            std::array<uint8_t, 2> padding{};
        };

        struct console_read_request
        {
            uint32_t events_read{};
            uint16_t flags{2};
            uint8_t unicode{1};
            uint8_t padding{};
        };

        struct console_input_record
        {
            uint16_t type;
            uint16_t padding;
            int32_t down;
            uint16_t repeat;
            uint16_t key;
            uint16_t scan;
            char16_t character;
            uint32_t control;
        };

        static_assert(sizeof(console_input_record) == 20);

        class ConsoleProtocolTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.audio = std::make_unique<null_audio_backend>();
                auto backend = std::make_unique<queued_console_backend>();
                this->input = backend.get();
                interfaces.console = std::move(backend);
                interfaces.ui = std::make_unique<null_ui_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                this->console_handle = this->emu->process.devices.store(io_device_container{u"Console", *this->emu, {}});
                this->emu->process.console_handle = this->console_handle;
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            template <typename T>
            NTSTATUS invoke(uint32_t api, T& data, handle endpoint = STDOUT_HANDLE)
            {
                console_mode_request request{};
                request.target_handle = endpoint.h;
                request.message_buffer_size = 8 + sizeof(T);
                request.message = this->storage + 0x100;
                request.data_size = sizeof(T);
                request.data = request.message + 8;
                this->emu->emu().write_memory(this->storage, request);
                this->emu->emu().write_memory(request.message, api);
                this->emu->emu().write_memory(request.message + 4, static_cast<uint32_t>(sizeof(T)));
                this->emu->emu().write_memory(request.data, data);
                io_device_context context{this->emu->emu()};
                context.source_handle = endpoint;
                context.io_control_code = 0x500016;
                context.input_buffer = this->storage;
                context.input_buffer_length = 48;
                const auto status = this->emu->process.devices.get(this->console_handle)->io_control(*this->emu, context);
                data = this->emu->emu().read_memory<T>(request.data);
                return status;
            }

            template <typename T>
            NTSTATUS invoke_text(uint32_t api, T& data, uint64_t text_size, bool output = false, handle endpoint = STDOUT_HANDLE,
                                 handle completion_event = {}, uint64_t apc = 0)
            {
                console_mode_request request{};
                request.target_handle = endpoint.h;
                request.input_count = output ? 1 : 2;
                request.output_count = output ? 2 : 1;
                request.message_buffer_size = 8 + sizeof(T);
                request.message = this->storage + 0x100;
                request.data_size = output ? sizeof(T) : text_size;
                request.data = output ? request.message + 8 : this->storage + 0x200;
                this->emu->emu().write_memory(this->storage, request);
                this->emu->emu().write_memory(this->storage + 48, static_cast<uint64_t>(output ? text_size : sizeof(T)));
                this->emu->emu().write_memory(this->storage + 56, output ? this->storage + 0x200 : request.message + 8);
                this->emu->emu().write_memory(request.message, api);
                this->emu->emu().write_memory(request.message + 4, static_cast<uint32_t>(sizeof(T)));
                this->emu->emu().write_memory(request.message + 8, data);
                io_device_context context{this->emu->emu()};
                context.source_handle = endpoint;
                context.io_control_code = 0x500016;
                context.input_buffer = this->storage;
                context.input_buffer_length = 64;
                context.io_status_block = {this->emu->memory, this->storage + 0x300};
                context.event = this->emu->process.reference_of(completion_event);
                context.event_low_bit = (completion_event.bits & 1) != 0;
                context.apc_routine = apc;
                context.apc_context = 0x1234;
                context.vcpu = &this->emu->vcpu(0);
                const auto status = this->emu->synchronize(
                    [&] { return this->emu->process.devices.get(this->console_handle)->execute_ioctl(*this->emu, context); });
                data = this->emu->emu().read_memory<T>(request.message + 8);
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            queued_console_backend* input{};
            handle console_handle{};
            uint64_t storage{};
        };

        class CooperativeConsoleReadTest : public ConsoleProtocolTest
        {
          protected:
            void SetUp() override
            {
                ConsoleProtocolTest::SetUp();
                this->emu->process.default_register_set = this->emu->vcpu(0).cpu.save_registers();
                auto& threads = this->emu->process.threads;
                this->reader =
                    threads.get(threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true}));
                this->worker =
                    threads.get(threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 12, true}));
                this->emu->vcpu(0).active_thread = this->reader;
                this->reader->setup_if_necessary(this->emu->vcpu(0).cpu, this->emu->process);
            }

            NTSTATUS read(const uint16_t flags = 0, const handle completion_event = {}, const uint64_t apc = 0)
            {
                console_read_request request{.flags = flags};
                return this->invoke_text(0x01000004, request, sizeof(console_input_record), true, STDIN_HANDLE, completion_event, apc);
            }

            NTSTATUS status() const
            {
                return this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + 0x300).Status;
            }

            NTSTATUS read_bytes(const ULONG length = 16, const handle completion_event = {}, const uint64_t apc = 0,
                                const uint64_t offset = 0)
            {
                const syscall_context context{
                    .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtReadFile(context, STDIN_HANDLE, completion_event.bits, apc, 0x1234,
                                                       {this->emu->memory, this->storage + 0x300 + offset}, this->storage + 0x200 + offset,
                                                       length, {this->emu->memory}, {this->emu->memory});
                });
            }

            NTSTATUS read_with_status_blocks(const uint64_t native_status, const uint64_t wow64_status, const handle completion_event,
                                             const uint64_t apc = 0)
            {
                io_device_context request{this->emu->memory};
                request.source_handle = STDIN_HANDLE;
                request.io_status_block.set_address(native_status);
                request.wow64_io_status_block.set_address(wow64_status);
                request.event = this->emu->process.reference_of(completion_event);
                request.apc_routine = apc;
                request.apc_context = 0x1234;
                request.output_buffer = this->storage + 0x200;
                request.output_buffer_length = 16;
                request.vcpu = &this->emu->vcpu(0);
                return this->emu->synchronize([&] { return read_console_input(*this->emu, request); });
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            NTSTATUS associate(const handle source, const handle port)
            {
                this->emu->memory.write_memory(storage + 0x740, port);
                this->emu->memory.write_memory(storage + 0x748, uint64_t{0xABCD});
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtSetInformationFile(context(), source, {this->emu->memory, storage + 0x750}, storage + 0x740,
                                                                 16, FileCompletionInformation);
                });
            }

            uint64_t transferred() const
            {
                return this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + 0x300).Information;
            }

            emulator_thread* reader{};
            emulator_thread* worker{};
        };
    }

    TEST_F(CooperativeConsoleReadTest, WaitingForInputLetsAnotherGuestThreadRun)
    {
        ASSERT_EQ(STATUS_PENDING, this->read());
        EXPECT_EQ(STATUS_PENDING, this->status());
        ASSERT_TRUE(this->reader->await_console_io);
        ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_EQ(this->worker, this->emu->vcpu(0).active_thread);
        EXPECT_EQ(STATUS_PENDING, this->status());

        this->input->queue("q");
        ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_EQ(this->reader, this->emu->vcpu(0).active_thread);
        EXPECT_FALSE(this->reader->await_console_io);
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(1u, this->emu->memory.read_memory<console_read_request>(this->storage + 0x108).events_read);
        EXPECT_EQ(u'q', this->emu->memory.read_memory<console_input_record>(this->storage + 0x200).character);
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, PendingPeekSignalsCompletionWithoutConsumingInput)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read(1, event));
        EXPECT_FALSE(this->emu->process.events.get(event)->is_signaled());
        EXPECT_FALSE(this->reader->await_console_io);
        this->input->queue("q");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
        const auto key = this->input->read_input_event(false, true);
        ASSERT_TRUE(key);
        EXPECT_EQ(u'q', key->character);
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, PendingReadCanBeCancelledWithoutConsumingInput)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read(0, event));
        auto* console = this->emu->process.devices.get(this->console_handle);
        EXPECT_TRUE(console->has_pending_io(this->reader->id));
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        EXPECT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtCancelIoFileEx(context, STDIN_HANDLE, {this->emu->memory, this->storage + 0x300},
                                                     {this->emu->memory, this->storage + 0x340});
        }));
        EXPECT_EQ(STATUS_CANCELLED, this->status());
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
        EXPECT_FALSE(console->has_pending_io(this->reader->id));
        this->input->queue("q");
        this->emu->synchronize([&] { console->work(*this->emu); });
        EXPECT_TRUE(this->input->input_available());
    }

    TEST_F(CooperativeConsoleReadTest, NonblockingReadWithNoInputCompletesImmediately)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->read(2));
        EXPECT_EQ(0u, this->emu->memory.read_memory<console_read_request>(this->storage + 0x108).events_read);
        EXPECT_FALSE(this->reader->await_console_io);
        EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, PendingReadSurvivesSnapshotRestore)
    {
        ASSERT_EQ(STATUS_PENDING, this->read());
        const auto reader_id = this->reader->id;
        utils::buffer_serializer snapshot{};
        this->emu->serialize(snapshot);
        utils::buffer_deserializer restore{snapshot};
        this->emu->deserialize(restore);
        auto* restored_reader = this->emu->process.find_thread_by_id(reader_id);
        ASSERT_NE(nullptr, restored_reader);
        EXPECT_TRUE(this->emu->process.devices.get(this->console_handle)->has_pending_io(reader_id));
        EXPECT_FALSE(this->emu->synchronize([&] { return restored_reader->is_thread_ready(*this->emu); }));
        this->input->queue("q");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_TRUE(this->emu->synchronize([&] { return restored_reader->is_thread_ready(*this->emu); }));
        EXPECT_EQ(u'q', this->emu->memory.read_memory<console_input_record>(this->storage + 0x200).character);
    }

    TEST_F(CooperativeConsoleReadTest, PendingEventReadCompletesWhenInputCloses)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read(0, event));
        this->input->close_input();
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_END_OF_FILE, this->status());
        EXPECT_EQ(0u, this->emu->memory.read_memory<console_read_request>(this->storage + 0x108).events_read);
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
        EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
        EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, ClosedEventInputDrainsSurrogatesAndTruncatedUtf8)
    {
        this->input->queue("\xF0\x9F\x98\x80\xE2\x82");
        this->input->close_input();
        for (const char16_t expected : {char16_t{0xD83D}, char16_t{0xDE00}, u'\uFFFD'})
        {
            ASSERT_EQ(STATUS_SUCCESS, this->read(1));
            EXPECT_EQ(expected, this->emu->memory.read_memory<console_input_record>(this->storage + 0x200).character);
            ASSERT_EQ(STATUS_SUCCESS, this->read());
            EXPECT_EQ(expected, this->emu->memory.read_memory<console_input_record>(this->storage + 0x200).character);
        }
        EXPECT_EQ(STATUS_END_OF_FILE, this->read());
        EXPECT_EQ(STATUS_SUCCESS, this->read(2));
        EXPECT_EQ(0u, this->emu->memory.read_memory<console_read_request>(this->storage + 0x108).events_read);
        EXPECT_FALSE(this->reader->await_console_io);
        EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
    }

    TEST_F(CooperativeConsoleReadTest, CompletionPortReadsKeepAsyncIssuingThreadRunnable)
    {
        const auto source = this->console_handle;
        const auto port = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->associate(source, port));
        for (const bool text : {false, true})
        {
            ASSERT_EQ(STATUS_PENDING, text ? this->read_bytes() : this->read());
            EXPECT_FALSE(this->reader->await_console_io);
            EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
            this->input->queue("q");
            this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
            EXPECT_EQ(STATUS_SUCCESS, this->status());
            const auto& queue = this->emu->process.io_completions.get(port)->queue;
            ASSERT_EQ(text ? 2u : 1u, queue.size());
            const auto& packet = queue.back();
            EXPECT_EQ(0xABCDu, packet.key_context);
            EXPECT_EQ(0x1234u, packet.apc_context);
            EXPECT_EQ(STATUS_SUCCESS, packet.io_status_block.Status);
            if (text)
            {
                EXPECT_EQ(1u, packet.io_status_block.Information);
            }
            else
            {
                EXPECT_EQ(u'q', this->emu->memory.read_memory<console_input_record>(this->storage + 0x200).character);
            }
        }
    }

    TEST_F(CooperativeConsoleReadTest, CompletionPortReadsRejectApcRoutines)
    {
        const auto source = this->console_handle;
        const auto port = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->associate(source, port));
        for (const bool text : {false, true})
        {
            ASSERT_EQ(STATUS_INVALID_PARAMETER, text ? this->read_bytes(16, {}, 0x5678) : this->read(0, {}, 0x5678));
            EXPECT_FALSE(this->reader->await_console_io);
            EXPECT_TRUE(this->reader->pending_apcs.empty());
            EXPECT_TRUE(this->emu->process.io_completions.get(port)->queue.empty());
            EXPECT_FALSE(this->emu->process.devices.get(source)->has_pending_io(this->reader->id));
        }
    }

    TEST_F(CooperativeConsoleReadTest, PendingEventReferenceSurvivesLastHandleCloseAndSnapshot)
    {
        auto& process = this->emu->process;
        const auto event_handle = process.events.store(sogen::event{});
        const auto reference = process.reference_of(event_handle);
        const auto handle_count = process.events.handle_count();
        ASSERT_EQ(STATUS_PENDING, this->read(0, event_handle));
        EXPECT_EQ(handle_count, process.events.handle_count());
        ASSERT_TRUE(process.events.erase(event_handle));
        ASSERT_NE(nullptr, process.get_object_as<sogen::event>(reference));

        utils::buffer_serializer snapshot{};
        this->emu->serialize(snapshot);
        utils::buffer_deserializer restore{snapshot};
        this->emu->deserialize(restore);

        auto observer = process.retain_object(reference);
        ASSERT_TRUE(observer);
        const auto cleanup = utils::finally([&] { process.release_object(*observer); });
        auto* event = process.get_object_as<sogen::event>(reference);
        ASSERT_NE(nullptr, event);
        EXPECT_FALSE(event->is_signaled());
        this->input->queue("q");
        this->emu->synchronize([&] { process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_TRUE(event->is_signaled());
        EXPECT_EQ(1u, event->ref_count);
        process.release_object(*observer);
        EXPECT_EQ(nullptr, process.get_object_as<sogen::event>(reference));
    }

    TEST_F(CooperativeConsoleReadTest, TaggedEventSignalsWithoutQueuingCompletionPacket)
    {
        const auto port = this->emu->process.io_completions.store(io_completion{});
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->console_handle, port));
        const auto tagged_event = make_handle(event.bits | 1);
        for (const bool text : {false, true})
        {
            ASSERT_EQ(STATUS_PENDING, text ? this->read_bytes(16, tagged_event) : this->read(0, tagged_event));
            EXPECT_FALSE(this->emu->process.events.get(event)->is_signaled());
            this->input->queue("q");
            this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
            EXPECT_EQ(STATUS_SUCCESS, this->status());
            EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
            EXPECT_TRUE(this->emu->process.io_completions.get(port)->queue.empty());
        }
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadLetsAnotherGuestThreadRunUntilDataArrives)
    {
        ASSERT_EQ(STATUS_PENDING, this->read_bytes());
        ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_EQ(this->worker, this->emu->vcpu(0).active_thread);
        EXPECT_EQ(STATUS_PENDING, this->status());
        this->input->queue("ab");
        ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
        EXPECT_EQ(this->reader, this->emu->vcpu(0).active_thread);
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(2u, this->transferred());
        EXPECT_EQ('a', this->emu->memory.read_memory<char>(this->storage + 0x200));
        EXPECT_EQ('b', this->emu->memory.read_memory<char>(this->storage + 0x201));
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadDrainsBufferedDataBeforeReportingEof)
    {
        this->input->queue("ab");
        this->input->close_input();
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes(1));
        EXPECT_EQ(1u, this->transferred());
        EXPECT_EQ('a', this->emu->memory.read_memory<char>(this->storage + 0x200));
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes());
        EXPECT_EQ(1u, this->transferred());
        EXPECT_EQ('b', this->emu->memory.read_memory<char>(this->storage + 0x200));
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes());
        EXPECT_EQ(0u, this->transferred());
        EXPECT_FALSE(this->reader->await_console_io);
    }

    TEST_F(CooperativeConsoleReadTest, ZeroLengthByteReadDoesNotWaitForInput)
    {
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes(0));
        EXPECT_EQ(0u, this->transferred());
        EXPECT_FALSE(this->reader->await_console_io);
        EXPECT_EQ(0u, this->input->blocking_refills);
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadsPreserveGuardPageProbeStatus)
    {
        for (const bool guarded_status : {false, true})
        {
            SCOPED_TRACE(guarded_status);
            const auto guarded = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_TRUE(this->emu->memory.protect_memory(
                guarded, 0x1000, nt_memory_permission(memory_permission::read_write, memory_permission_ext::guard)));
            const auto status_address = guarded_status ? guarded : this->storage + 0x300;
            const auto output_address = guarded_status ? this->storage + 0x200 : guarded;
            const auto read = [&] {
                return this->emu->synchronize([&] {
                    return syscalls::handle_NtReadFile(this->context(), STDIN_HANDLE, 0, 0, 0, {this->emu->memory, status_address},
                                                       output_address, 16, {this->emu->memory}, {this->emu->memory});
                });
            };
            EXPECT_EQ(STATUS_GUARD_PAGE_VIOLATION, read());
            EXPECT_FALSE(this->emu->memory.get_region_info(guarded).permissions.is_guarded());
            EXPECT_FALSE(this->reader->await_console_io);
            EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
            this->input->queue("q");
            ASSERT_EQ(STATUS_SUCCESS, read());
            EXPECT_EQ('q', this->emu->memory.read_memory<char>(output_address));
        }
    }

    TEST_F(CooperativeConsoleReadTest, CancelledByteReadDoesNotConsumeLaterInput)
    {
        ASSERT_EQ(STATUS_PENDING, this->read_bytes());
        const syscall_context context{
            .win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
        ASSERT_EQ(STATUS_SUCCESS, this->emu->synchronize([&] {
            return syscalls::handle_NtCancelIoFileEx(context, STDIN_HANDLE, {this->emu->memory, this->storage + 0x300},
                                                     {this->emu->memory, this->storage + 0x340});
        }));
        EXPECT_EQ(STATUS_CANCELLED, this->status());
        EXPECT_EQ(0u, this->transferred());
        EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
        this->input->queue("q");
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes());
        EXPECT_EQ(1u, this->transferred());
        EXPECT_EQ('q', this->emu->memory.read_memory<char>(this->storage + 0x200));
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadSignalsEventAndQueuesCompletionApc)
    {
        const auto completion = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, completion, 0x5678));
        EXPECT_FALSE(this->reader->await_console_io);
        EXPECT_FALSE(this->emu->process.events.get(completion)->is_signaled());
        this->input->queue("ab");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(2u, this->transferred());
        EXPECT_TRUE(this->emu->process.events.get(completion)->is_signaled());
        ASSERT_EQ(1u, this->reader->pending_apcs.size());
        const auto& apc = this->reader->pending_apcs.front();
        EXPECT_EQ(0x5678u, apc.apc_routine);
        EXPECT_EQ(0x1234u, apc.apc_argument1);
        EXPECT_EQ(this->storage + 0x300, apc.apc_argument2);
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadWithApcDoesNotParkItsIssuingThread)
    {
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, {}, 0x5678));
        EXPECT_FALSE(this->reader->await_console_io);
        this->input->close_input();
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(0u, this->transferred());
        EXPECT_EQ(1u, this->reader->pending_apcs.size());
    }

    TEST_F(CooperativeConsoleReadTest, GuestStatusWritesDoNotCompleteSynchronousReads)
    {
        for (const bool text : {false, true})
        {
            SCOPED_TRACE(text);
            ASSERT_EQ(STATUS_PENDING, text ? this->read_bytes() : this->read());
            this->emu->memory.write_memory(this->storage + 0x300, NTSTATUS{STATUS_SUCCESS});
            EXPECT_FALSE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
            this->input->queue("q");
            this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
            EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
            EXPECT_EQ(STATUS_SUCCESS, this->reader->pending_status);
            this->reader->pending_status.reset();
        }
    }

    TEST_F(CooperativeConsoleReadTest, AsyncCompletionDoesNotReleaseSynchronousRead)
    {
        const auto event = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, event));
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, {}, 0, 0x100));
        this->input->queue("a");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
        EXPECT_FALSE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
        this->input->queue("b");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
        EXPECT_EQ(STATUS_SUCCESS, this->reader->pending_status);
        EXPECT_EQ('a', this->emu->memory.read_memory<char>(this->storage + 0x200));
        EXPECT_EQ('b', this->emu->memory.read_memory<char>(this->storage + 0x300));
    }

    TEST_F(CooperativeConsoleReadTest, SynchronousReadsCompleteAfterStatusBlocksAreUnmapped)
    {
        enum class completion
        {
            cancellation,
            data,
            eof,
        };
        for (const auto result : {completion::cancellation, completion::data, completion::eof})
        {
            SCOPED_TRACE(static_cast<int>(result));
            const auto native = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            const auto wow64 =
                this->emu->memory.allocate_memory(0x1000, memory_permission::read_write, false, DEFAULT_ALLOCATION_ADDRESS_32BIT);
            ASSERT_EQ(STATUS_PENDING, this->read_with_status_blocks(native, wow64, {}));
            ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
            ASSERT_EQ(this->worker, this->emu->vcpu(0).active_thread);
            ASSERT_TRUE(this->emu->memory.release_memory(native, 0));
            ASSERT_TRUE(this->emu->memory.release_memory(wow64, 0));
            auto* console = this->emu->process.devices.get(this->console_handle);
            ASSERT_NO_THROW(this->emu->synchronize([&] { console->work(*this->emu); }));
            EXPECT_FALSE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
            ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
            EXPECT_EQ(this->worker, this->emu->vcpu(0).active_thread);
            if (result == completion::cancellation)
            {
                ASSERT_TRUE(this->emu->synchronize([&] { return console->cancel_io(*this->emu, native); }));
            }
            else
            {
                if (result == completion::eof)
                {
                    this->input->close_input();
                }
                else
                {
                    this->input->queue("ab");
                }
                ASSERT_NO_THROW(this->emu->synchronize([&] { console->work(*this->emu); }));
                if (result == completion::data)
                {
                    EXPECT_EQ('a', this->emu->memory.read_memory<char>(this->storage + 0x200));
                    EXPECT_EQ('b', this->emu->memory.read_memory<char>(this->storage + 0x201));
                }
            }
            EXPECT_FALSE(console->has_pending_io(this->reader->id));
            EXPECT_FALSE(this->reader->await_console_io);
            EXPECT_TRUE(this->emu->synchronize([&] { return this->reader->is_thread_ready(*this->emu); }));
            EXPECT_EQ(result == completion::cancellation ? STATUS_CANCELLED : STATUS_SUCCESS, this->reader->pending_status);
            ASSERT_TRUE(this->emu->perform_thread_switch(this->emu->vcpu(0)));
            EXPECT_EQ(this->reader, this->emu->vcpu(0).active_thread);
        }
    }

    TEST_F(CooperativeConsoleReadTest, UnmappedStatusBlocksDoNotPreventEventAndApcCompletion)
    {
        for (const unsigned unmapped : {1u, 2u, 3u})
        {
            SCOPED_TRACE(unmapped);
            const auto native = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            const auto wow64 =
                this->emu->memory.allocate_memory(0x1000, memory_permission::read_write, false, DEFAULT_ALLOCATION_ADDRESS_32BIT);
            const auto event = this->emu->process.events.store(sogen::event{});
            this->reader->pending_apcs.clear();
            ASSERT_EQ(STATUS_PENDING, this->read_with_status_blocks(native, wow64, event, 0x5678));
            if (unmapped & 1u)
            {
                ASSERT_TRUE(this->emu->memory.release_memory(native, 0));
            }
            if (unmapped & 2u)
            {
                ASSERT_TRUE(this->emu->memory.release_memory(wow64, 0));
            }
            this->input->queue("ab");
            ASSERT_NO_THROW(this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); }));
            EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
            ASSERT_EQ(1u, this->reader->pending_apcs.size());
            const auto& apc = this->reader->pending_apcs.front();
            EXPECT_EQ(0x5678u, apc.apc_routine);
            EXPECT_EQ(0x1234u, apc.apc_argument1);
            EXPECT_EQ(wow64, apc.apc_argument2);
            EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
            EXPECT_EQ('a', this->emu->memory.read_memory<char>(this->storage + 0x200));
            EXPECT_EQ('b', this->emu->memory.read_memory<char>(this->storage + 0x201));
            if (!(unmapped & 1u))
            {
                const auto block = this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(native);
                EXPECT_EQ(STATUS_SUCCESS, block.Status);
                EXPECT_EQ(2u, block.Information);
            }
            if (!(unmapped & 2u))
            {
                const auto block = this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu32>>>(wow64);
                EXPECT_EQ(STATUS_SUCCESS, block.Status);
                EXPECT_EQ(2u, block.Information);
            }
        }
    }

    TEST_F(CooperativeConsoleReadTest, CompletionPacketsUseResultAfterStatusBlocksAreUnmapped)
    {
        const auto port = this->emu->process.io_completions.store(io_completion{});
        ASSERT_EQ(STATUS_SUCCESS, this->associate(this->console_handle, port));
        for (const bool eof : {false, true})
        {
            SCOPED_TRACE(eof);
            const auto native = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
            const auto wow64 =
                this->emu->memory.allocate_memory(0x1000, memory_permission::read_write, false, DEFAULT_ALLOCATION_ADDRESS_32BIT);
            const auto event = this->emu->process.events.store(sogen::event{});
            ASSERT_EQ(STATUS_PENDING, this->read_with_status_blocks(native, wow64, event));
            ASSERT_TRUE(this->emu->memory.release_memory(native, 0));
            ASSERT_TRUE(this->emu->memory.release_memory(wow64, 0));
            if (eof)
            {
                this->input->close_input();
            }
            else
            {
                this->input->queue("ab");
            }
            ASSERT_NO_THROW(this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); }));
            EXPECT_TRUE(this->emu->process.events.get(event)->is_signaled());
            EXPECT_FALSE(this->emu->process.devices.get(this->console_handle)->has_pending_io(this->reader->id));
            const auto& queue = this->emu->process.io_completions.get(port)->queue;
            ASSERT_EQ(eof ? 2u : 1u, queue.size());
            const auto& packet = queue.back();
            EXPECT_EQ(0xABCDu, packet.key_context);
            EXPECT_EQ(0x1234u, packet.apc_context);
            EXPECT_EQ(STATUS_SUCCESS, packet.io_status_block.Status);
            EXPECT_EQ(eof ? 0u : 2u, packet.io_status_block.Information);
        }
    }

    TEST_F(CooperativeConsoleReadTest, ByteReadSurvivesSnapshotRestore)
    {
        ASSERT_EQ(STATUS_PENDING, this->read_bytes());
        const auto reader_id = this->reader->id;
        utils::buffer_serializer snapshot{};
        this->emu->serialize(snapshot);
        utils::buffer_deserializer restore{snapshot};
        this->emu->deserialize(restore);
        auto* restored_reader = this->emu->process.find_thread_by_id(reader_id);
        ASSERT_NE(nullptr, restored_reader);
        EXPECT_FALSE(this->emu->synchronize([&] { return restored_reader->is_thread_ready(*this->emu); }));
        this->input->queue("q");
        this->emu->synchronize([&] { this->emu->process.devices.get(this->console_handle)->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(1u, this->transferred());
        EXPECT_TRUE(this->emu->synchronize([&] { return restored_reader->is_thread_ready(*this->emu); }));
        EXPECT_EQ('q', this->emu->memory.read_memory<char>(this->storage + 0x200));
    }

    TEST_F(CooperativeConsoleReadTest, ByteCancellationWaitsForHostReadToStop)
    {
        this->input->cancellation_pending = true;
        const auto completion = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, completion));
        auto* console = this->emu->process.devices.get(this->console_handle);
        EXPECT_TRUE(this->emu->synchronize([&] { return console->cancel_io(*this->emu, this->storage + 0x300); }));
        EXPECT_EQ(STATUS_PENDING, this->status());
        EXPECT_FALSE(this->emu->process.events.get(completion)->is_signaled());
        this->input->queue("q");
        this->emu->synchronize([&] { console->work(*this->emu); });
        EXPECT_EQ(STATUS_PENDING, this->status());
        this->input->cancellation_pending = false;
        this->emu->synchronize([&] { console->work(*this->emu); });
        EXPECT_EQ(STATUS_CANCELLED, this->status());
        EXPECT_EQ(0u, this->transferred());
        EXPECT_TRUE(this->emu->process.events.get(completion)->is_signaled());
        EXPECT_EQ(std::optional<std::string>{"q"}, this->input->try_read_input(16));
    }

    TEST_F(CooperativeConsoleReadTest, CancellingQueuedByteReadLeavesActiveReadAlone)
    {
        const auto first = this->emu->process.events.store(sogen::event{});
        const auto second = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, first));
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, second, 0, 0x100));
        auto* console = this->emu->process.devices.get(this->console_handle);
        ASSERT_TRUE(this->emu->synchronize([&] { return console->cancel_io(*this->emu, this->storage + 0x400); }));
        EXPECT_EQ(0u, this->input->cancellations);
        EXPECT_EQ(STATUS_PENDING, this->status());
        EXPECT_TRUE(this->emu->process.events.get(second)->is_signaled());
        this->input->queue("q");
        this->emu->synchronize([&] { console->work(*this->emu); });
        EXPECT_EQ(STATUS_SUCCESS, this->status());
        EXPECT_EQ(1u, this->transferred());
        EXPECT_EQ('q', this->emu->memory.read_memory<char>(this->storage + 0x200));
        EXPECT_TRUE(this->emu->process.events.get(first)->is_signaled());
        EXPECT_EQ(STATUS_CANCELLED, this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + 0x400).Status);
    }

    TEST_F(CooperativeConsoleReadTest, ZeroLengthByteReadCompletesBehindPendingRead)
    {
        const auto completion = this->emu->process.events.store(sogen::event{});
        ASSERT_EQ(STATUS_PENDING, this->read_bytes(16, completion));
        ASSERT_EQ(STATUS_SUCCESS, this->read_bytes(0, {}, 0, 0x100));
        EXPECT_EQ(STATUS_PENDING, this->status());
        EXPECT_FALSE(this->emu->process.events.get(completion)->is_signaled());
        const auto empty = this->emu->memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + 0x400);
        EXPECT_EQ(0u, empty.Information);
        EXPECT_EQ(STATUS_SUCCESS, empty.Status);
    }

    TEST_F(ConsoleProtocolTest, PunctuationAndUnicodeDoNotBecomeNavigationKeys)
    {
        for (const auto character : std::u16string_view{u"\"&.-()%'[]\u03BB"})
        {
            this->input->queue(u16_to_u8(std::u16string_view{&character, 1}));
            console_read_request request{};
            ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x01000004, request, sizeof(console_input_record), true, STDIN_HANDLE));
            ASSERT_EQ(1u, request.events_read);
            const auto record = this->emu->emu().read_memory<console_input_record>(this->storage + 0x200);
            EXPECT_EQ(character, record.character);
            EXPECT_EQ(0u, record.key);
        }
    }

    TEST_F(ConsoleProtocolTest, LettersDigitsAndSpaceRetainTheirVirtualKeys)
    {
        this->input->queue("aA09 ");
        for (const auto key : std::u16string_view{u"AA09 "})
        {
            console_read_request request{};
            ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x01000004, request, sizeof(console_input_record), true, STDIN_HANDLE));
            ASSERT_EQ(1u, request.events_read);
            EXPECT_EQ(key, this->emu->emu().read_memory<console_input_record>(this->storage + 0x200).key);
        }
    }

    TEST_F(ConsoleProtocolTest, ModifiedKeysRetainControlInGuestRecords)
    {
        this->input->queue("\x1B[13;5u\x1B[9;5u\x1B[127;5u\x1B[2;5~\x1B[3;5~\x1B[5;5~\x1B[6;5~");
        for (const auto key : {VK_RETURN, VK_TAB, VK_BACK, VK_INSERT, VK_DELETE, VK_PRIOR, VK_NEXT})
        {
            console_read_request request{};
            ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x01000004, request, sizeof(console_input_record), true, STDIN_HANDLE));
            ASSERT_EQ(1u, request.events_read);
            const auto record = this->emu->emu().read_memory<console_input_record>(this->storage + 0x200);
            EXPECT_EQ(key, record.key);
            EXPECT_NE(0u, record.control & 0x0008);
        }
    }

    TEST_F(ConsoleProtocolTest, FontUpdatesSurviveSerialization)
    {
        console_font_request font{};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x03000004, font));
        EXPECT_GT(font.width, 0);
        EXPECT_GT(font.height, 0);
        EXPECT_EQ(u"Consolas", std::u16string(font.face_name.data()));
        font.width = 12;
        font.height = 24;
        font.weight = 700;
        ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x0300002C, font));
        auto* console = this->emu->process.devices.get(this->console_handle);
        utils::buffer_serializer state{};
        console->serialize(state);
        font.width = 8;
        ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x0300002C, font));
        utils::buffer_deserializer reader{state};
        reader.register_factory<windows_emulator_wrapper>([&] { return windows_emulator_wrapper{*this->emu}; });
        console->deserialize(reader);
        font = {};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x03000004, font));
        EXPECT_EQ(12, font.width);
        EXPECT_EQ(24, font.height);
        EXPECT_EQ(700u, font.weight);
    }

    TEST_F(ConsoleProtocolTest, WritesUnicodeAndAnsiTextUsingSeparateInputAndDescriptorBuffers)
    {
        std::string output;
        this->emu->callbacks.on_stdout = [&](std::string_view text) { output.append(text); };
        constexpr std::array<char16_t, 3> wide{u'A', u'\u03bb', u'B'};
        this->emu->emu().write_memory(this->storage + 0x200, wide.data(), sizeof(wide));
        console_text_request request{.unicode = 1};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x01000006, request, sizeof(wide)));
        EXPECT_EQ("A\xCE\xBB"
                  "B",
                  output);
        EXPECT_EQ(sizeof(wide), request.bytes_written);
        constexpr std::string_view narrow = "plain-output";
        this->emu->emu().write_memory(this->storage + 0x200, narrow.data(), narrow.size());
        request = {};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x01000006, request, narrow.size()));
        EXPECT_EQ("A\xCE\xBB"
                  "Bplain-output",
                  output);
        EXPECT_EQ(narrow.size(), request.bytes_written);
    }

    TEST_F(ConsoleProtocolTest, RejectsOddUnicodeTextLengthAndInputEndpoint)
    {
        console_text_request request{.unicode = 1};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke_text(0x01000006, request, 3));
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke_text(0x01000006, request, 2, false, STDIN_HANDLE));
        EXPECT_EQ(0u, request.bytes_written);
    }

    TEST_F(ConsoleProtocolTest, ConsoleTitleRoundTripsAndSurvivesSerialization)
    {
        constexpr std::array<char16_t, 3> title{u'A', u'\u03bb', u'B'};
        this->emu->emu().write_memory(this->storage + 0x200, title.data(), sizeof(title));
        uint8_t unicode = 1;
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x02000015, unicode, sizeof(title)));
        auto* console = this->emu->process.devices.get(this->console_handle);
        utils::buffer_serializer state{};
        console->serialize(state);
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x02000015, unicode, 0));
        utils::buffer_deserializer reader{state};
        reader.register_factory<windows_emulator_wrapper>([&] { return windows_emulator_wrapper{*this->emu}; });
        console->deserialize(reader);
        console_title_request request{.unicode = 1};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x02000014, request, 8, true));
        EXPECT_EQ(3u, request.length);
        EXPECT_EQ((std::array<char16_t, 4>{u'A', u'\u03bb', u'B', u'\0'}),
                  (this->emu->emu().read_memory<std::array<char16_t, 4>>(this->storage + 0x200)));
        EXPECT_EQ(6u, (this->emu->emu().read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(this->storage + 0x300).Information));
        ASSERT_EQ(STATUS_SUCCESS, this->invoke_text(0x02000014, request, 4, true));
        EXPECT_EQ(3u, request.length);
        EXPECT_EQ((std::array<char16_t, 2>{u'A', u'\0'}), (this->emu->emu().read_memory<std::array<char16_t, 2>>(this->storage + 0x200)));
    }

    TEST_F(ConsoleProtocolTest, FontRequestsRejectInputHandlesAndInvalidSizes)
    {
        console_font_request font{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke(0x03000004, font, STDIN_HANDLE));
        font.width = -1;
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke(0x0300002C, font));
        uint32_t truncated{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke(0x03000004, truncated));
    }

    TEST_F(ConsoleProtocolTest, LargestWindowMatchesScreenBufferDimensions)
    {
        for (const auto dimensions : {console_size{}, console_size{.width = 79, .height = 17}, console_size{.width = 113, .height = 31},
                                      console_size{.width = 1, .height = 1}})
        {
            this->input->size_ = dimensions;
            const std::array<int16_t, 2> expected{dimensions.width, dimensions.height};
            std::array<int16_t, 2> largest{};
            ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x0200000B, largest));
            EXPECT_EQ(expected, largest);
            console_buffer_info info{};
            ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x02000007, info));
            EXPECT_EQ(expected, info.size);
            EXPECT_EQ(expected, info.window_size);
            EXPECT_EQ(expected, info.maximum_window_size);
            EXPECT_EQ((std::array<int16_t, 2>{}), info.window_origin);
        }
    }

    TEST_F(ConsoleProtocolTest, BufferInfoRejectsInputHandlesAndInvalidSizes)
    {
        console_buffer_info info{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke(0x02000007, info, STDIN_HANDLE));
        uint32_t truncated{};
        EXPECT_EQ(STATUS_INVALID_PARAMETER, this->invoke(0x02000007, truncated));
    }

    TEST(ConsoleBackendTest, HeadlessConsoleRetainsDefaultDimensions)
    {
        null_console_backend console{};
        EXPECT_EQ(80, console.get_size().width);
        EXPECT_EQ(25, console.get_size().height);
    }

    TEST(ConsoleBackendTest, EventsAndByteReadsShareBufferedInput)
    {
        queued_console_backend console{};
        console.queue("\x1B[Aq");

        ASSERT_TRUE(console.input_available());
        const auto event = console.read_input_event(false, true);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(event->key, console_key::up);
        EXPECT_EQ(console.read_input(1), "q");
        EXPECT_FALSE(console.input_available());
    }

    TEST(ConsoleBackendTest, SurrogateEventsRemainReadyUntilConsumed)
    {
        queued_console_backend console{};
        console.queue("\xF0\x9F\x98\x80");
        const auto high = console.read_input_event(false, true);
        ASSERT_TRUE(high.has_value());
        EXPECT_EQ(0xD83D, high->character);
        EXPECT_TRUE(console.input_available());
        const auto low = console.read_input_event(false, true);
        ASSERT_TRUE(low.has_value());
        EXPECT_EQ(0xDE00, low->character);
        EXPECT_FALSE(console.input_available());
    }

    TEST(ConsoleBackendTest, DecodesEnterAndDownNavigation)
    {
        queued_console_backend console{};
        console.queue("\r\x1B[B");

        const auto enter = console.read_input_event(false, true);
        ASSERT_TRUE(enter.has_value());
        EXPECT_EQ(enter->key, console_key::enter);
        EXPECT_EQ(enter->character, u'\r');

        const auto down = console.read_input_event(false, true);
        ASSERT_TRUE(down.has_value());
        EXPECT_EQ(down->key, console_key::down);
    }

    TEST(ConsoleBackendTest, DecodesControlModifiedCharacters)
    {
        struct character_key
        {
            char16_t character;
            console_key expected_key;
            char16_t expected_character;
        };

        constexpr std::array keys{
            character_key{.character = u'\r', .expected_key = console_key::enter, .expected_character = u'\r'},
            character_key{.character = u'\n', .expected_key = console_key::enter, .expected_character = u'\r'},
            character_key{.character = u'\t', .expected_key = console_key::tab, .expected_character = u'\t'},
            character_key{.character = u'\b', .expected_key = console_key::backspace, .expected_character = u'\b'},
            character_key{.character = 0x7F, .expected_key = console_key::backspace, .expected_character = u'\b'},
            character_key{.character = u'.', .expected_key = console_key::character, .expected_character = u'.'},
            character_key{.character = u'\u03BB', .expected_key = console_key::character, .expected_character = u'\u03BB'},
            character_key{.character = u'\x1B', .expected_key = console_key::escape, .expected_character = 0},
            character_key{.character = u'\x03', .expected_key = console_key::character, .expected_character = u'\x03'},
        };
        queued_console_backend console{};
        for (const auto& key : keys)
        {
            console.queue("\x1B[" + std::to_string(static_cast<uint16_t>(key.character)) + ";5u");
            for (const bool remove : {false, true})
            {
                const auto event = console.read_input_event(false, remove);
                ASSERT_TRUE(event.has_value());
                EXPECT_EQ(key.expected_key, event->key);
                EXPECT_EQ(key.expected_character, event->character);
                EXPECT_TRUE(event->control);
            }
            EXPECT_FALSE(console.input_available());
        }
    }

    TEST(ConsoleBackendTest, DecodesControlModifiedTildeNavigation)
    {
        constexpr std::array keys{
            console_key::home,    console_key::insert,    console_key::delete_key, console_key::end,
            console_key::page_up, console_key::page_down, console_key::home,       console_key::end,
        };
        queued_console_backend console{};
        for (size_t i = 0; i < keys.size(); ++i)
        {
            console.queue("\x1B[" + std::to_string(i + 1) + ";5~");
            for (const bool remove : {false, true})
            {
                const auto event = console.read_input_event(false, remove);
                ASSERT_TRUE(event.has_value());
                EXPECT_EQ(keys.at(i), event->key);
                EXPECT_TRUE(event->control);
            }
            EXPECT_FALSE(console.input_available());
        }
    }

    TEST(ConsoleBackendTest, MalformedModifiedCharactersPreserveInput)
    {
        queued_console_backend console{};
        for (const auto* const sequence : {"[;5u", "[65536;5u", "[9999999999;5u", "[13;5;5u", "[13;u", "[13;5"})
        {
            console.queue("\x1B" + std::string(sequence));
            const auto event = console.read_input_event(false, true);
            ASSERT_TRUE(event.has_value());
            EXPECT_EQ(console_key::escape, event->key);
            EXPECT_EQ(sequence, console.read_input(32));
            EXPECT_FALSE(console.input_available());
        }
    }

    TEST(ConsoleBackendTest, RestoresSerializedInputModeAfterBackendReset)
    {
        const auto emulation_root =
            std::filesystem::temp_directory_path() /
            ("sogen-console-backend-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(emulation_root / "filesys");
        const auto cleanup = utils::finally([&] {
            std::error_code error{};
            std::filesystem::remove_all(emulation_root, error);
        });

        emulator_settings settings{};
        settings.emulation_root = emulation_root;
        settings.load_registry = false;

        auto backend = std::make_unique<recording_console_backend>();
        auto* backend_ptr = backend.get();

        emulator_interfaces interfaces{};
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::move(backend);
        interfaces.ui = std::make_unique<null_ui_backend>();

        windows_emulator emu{create_x86_64_emulator_from_environment(), settings, {}, std::move(interfaces)};

        io_device_container console{u"Console", emu, {}};
        const auto console_handle = emu.process.devices.store(std::move(console));
        emu.process.console_handle = console_handle;

        auto* restored_console = emu.process.devices.get(console_handle);
        ASSERT_NE(restored_console, nullptr);

        const auto request_address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(request_address, 0u);
        console_mode_request request{};
        request.target_handle = STDIN_HANDLE.h;
        request.message_buffer_size = sizeof(request.api_number) + sizeof(request.message_data_size) + sizeof(request.mode);
        request.message = request_address + offsetof(console_mode_request, api_number);
        request.data_size = sizeof(request.mode);
        request.data = request_address + offsetof(console_mode_request, mode);
        request.api_number = 0x01000002;
        request.message_data_size = sizeof(request.mode);

        io_device_context context{emu.emu()};
        context.source_handle = STDIN_HANDLE;
        context.io_control_code = 0x500016;
        context.input_buffer = request_address;
        context.input_buffer_length = sizeof(request);

        request.mode = 0x0001;
        emu.emu().write_memory(request_address, &request, sizeof(request));
        ASSERT_EQ(restored_console->io_control(emu, context), STATUS_SUCCESS);
        utils::buffer_serializer state{};
        restored_console->serialize(state);

        request.mode = 0x0007;
        emu.emu().write_memory(request_address, &request, sizeof(request));
        ASSERT_EQ(restored_console->io_control(emu, context), STATUS_SUCCESS);

        utils::buffer_deserializer deserializer{state};
        deserializer.register_factory<windows_emulator_wrapper>([&] { return windows_emulator_wrapper{emu}; });
        restored_console->deserialize(deserializer);

        emu.console().reset();
        ASSERT_EQ(backend_ptr->reset_count_, 1u);

        emu.process.restore_after_state_restore(emu);

        EXPECT_TRUE(backend_ptr->mode_.processed);
        EXPECT_FALSE(backend_ptr->mode_.line);
        EXPECT_FALSE(backend_ptr->mode_.echo);
    }
} // namespace sogen::test
