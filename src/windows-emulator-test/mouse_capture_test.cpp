#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        class MouseCaptureTest : public testing::Test
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

                const auto base = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                this->emu->process.base_allocator = emulator_allocator{this->emu->memory, base, 0x10000};
                this->emu->process.user_handles.setup(false);
                this->thread_handle =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
            }

            uint32_t capture_count() const
            {
                return this->emu->process.user_handles.get_server_info().read().captureCount;
            }

            hwnd create_window(const uint32_t thread_id)
            {
                auto [window_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.thread_id = thread_id;
                win.handle = window_handle.bits;
                return win.handle;
            }

            std::unique_ptr<windows_emulator> emu{};
            handle thread_handle{};
        };
    }

    TEST_F(MouseCaptureTest, TerminatingTheOwnerThreadReleasesTheCapture)
    {
        auto& process = this->emu->process;
        auto* thread = process.threads.get(this->thread_handle);
        ASSERT_NE(nullptr, thread);

        const auto window_handle = this->create_window(thread->id);
        process.set_mouse_capture(window_handle);
        ASSERT_EQ(window_handle, process.mouse_capture_window);
        ASSERT_EQ(1u, this->capture_count());

        process.terminate_thread(*thread, STATUS_SUCCESS);

        EXPECT_EQ(hwnd{}, process.mouse_capture_window);
        EXPECT_EQ(0u, this->capture_count());
    }

    TEST_F(MouseCaptureTest, ReleasingAnotherWindowKeepsTheCapture)
    {
        auto& process = this->emu->process;
        auto* thread = process.threads.get(this->thread_handle);
        ASSERT_NE(nullptr, thread);

        const auto capturing = this->create_window(thread->id);
        const auto other = this->create_window(thread->id);
        process.set_mouse_capture(capturing);

        process.release_mouse_capture(other);

        EXPECT_EQ(capturing, process.mouse_capture_window);
        EXPECT_EQ(1u, this->capture_count());
    }
}
