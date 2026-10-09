#include <console_backends/buffered_console_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>

#include <backend_selection.hpp>
#include <io_device.hpp>
#include <windows_emulator.hpp>
#include <utils/finally.hpp>

#include <gtest/gtest.h>

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
