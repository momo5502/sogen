#include <console_backends/buffered_console_backend.hpp>

#include <array>
#include <chrono>

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
            void refill(int /*timeout_ms*/) override
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

            std::optional<console_key_event> read_input_event() override
            {
                return std::nullopt;
            }

            std::string read_input(size_t /*length*/) override
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
    }

    TEST(ConsoleBackendTest, EventsAndByteReadsShareBufferedInput)
    {
        queued_console_backend console{};
        console.queue("\x1B[Aq");

        ASSERT_TRUE(console.input_available());
        const auto event = console.read_input_event();
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(event->key, console_key::up);
        EXPECT_EQ(console.read_input(1), "q");
        EXPECT_FALSE(console.input_available());
    }

    TEST(ConsoleBackendTest, DecodesEnterAndDownNavigation)
    {
        queued_console_backend console{};
        console.queue("\r\x1B[B");

        const auto enter = console.read_input_event();
        ASSERT_TRUE(enter.has_value());
        EXPECT_EQ(enter->key, console_key::enter);
        EXPECT_EQ(enter->character, u'\r');

        const auto down = console.read_input_event();
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

        utils::buffer_serializer state{};
        state.write(uint32_t{1});
        state.write(false);
        state.write_string(std::u16string_view{u"Console"});
        state.write(uint32_t{1});
        state.write(uint16_t{7});
        state.write(std::array<int16_t, 2>{});
        state.write(uint32_t{0x0079});
        state.write(uint32_t{0x0003});

        utils::buffer_deserializer deserializer{state};
        auto* restored_console = emu.process.devices.get(console_handle);
        ASSERT_NE(restored_console, nullptr);
        restored_console->deserialize(deserializer);

        emu.console().reset();
        ASSERT_EQ(backend_ptr->reset_count_, 1u);

        emu.process.restore_after_state_restore(emu);

        EXPECT_TRUE(backend_ptr->mode_.processed);
        EXPECT_FALSE(backend_ptr->mode_.line);
        EXPECT_FALSE(backend_ptr->mode_.echo);
    }
} // namespace sogen::test
