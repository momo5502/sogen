#include <console_backends/buffered_console_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>

#include <backend_selection.hpp>
#include <io_device.hpp>
#include <windows_emulator.hpp>
#include <utils/finally.hpp>

#include <gtest/gtest.h>
#include "emulation_test_utils.hpp"

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

            void set_input_mode(const console_input_mode& mode) override
            {
                mode_ = mode;
            }

            console_input_mode mode_{};

          protected:
            void refill(int) override
            {
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
            NTSTATUS invoke_text(uint32_t api, T& data, uint64_t text_size, bool output = false, handle endpoint = STDOUT_HANDLE)
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
                const auto status = this->emu->process.devices.get(this->console_handle)->io_control(*this->emu, context);
                data = this->emu->emu().read_memory<T>(request.message + 8);
                return status;
            }

            std::unique_ptr<windows_emulator> emu{};
            queued_console_backend* input{};
            handle console_handle{};
            uint64_t storage{};
        };
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
        std::array<int16_t, 2> size{};
        ASSERT_EQ(STATUS_SUCCESS, this->invoke(0x0200000B, size));
        EXPECT_EQ(80, size[0]);
        EXPECT_EQ(25, size[1]);
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
        restored_console->deserialize(deserializer);

        emu.console().reset();
        ASSERT_EQ(backend_ptr->reset_count_, 1u);

        emu.process.restore_after_state_restore(emu);

        EXPECT_TRUE(backend_ptr->mode_.processed);
        EXPECT_FALSE(backend_ptr->mode_.line);
        EXPECT_FALSE(backend_ptr->mode_.echo);
    }
} // namespace sogen::test
