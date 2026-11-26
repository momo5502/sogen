#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t ws_child = 0x40000000;
        constexpr uint32_t ws_visible = 0x10000000;

        class PointerRoutingTest : public testing::Test
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
                this->first_handle = this->create_thread(8);
                this->second_handle = this->create_thread(12);
            }

            handle create_thread(const uint32_t id)
            {
                return this->emu->process.threads.store(
                    emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, id, true});
            }

            emulator_thread& thread(const handle thread_handle)
            {
                return *this->emu->process.threads.get(thread_handle);
            }

            hwnd create_window(const uint32_t thread_id, const hwnd parent, const uint32_t style, const int32_t x, const int32_t y,
                               const int32_t width, const int32_t height)
            {
                auto [window_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.thread_id = thread_id;
                win.handle = window_handle.bits;
                win.parent_handle = parent;
                win.class_name = u"Window";
                win.style = style;
                win.x = x;
                win.y = y;
                win.width = width;
                win.height = height;
                return win.handle;
            }

            void send_pointer(const hwnd window, const uint32_t message, const int32_t x, const int32_t y, const uint64_t w_param = 0)
            {
                const auto l_param = static_cast<uint16_t>(x) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16);
                this->emu->handle_ui_event({.window = window, .message = message, .wParam = w_param, .lParam = l_param});
            }

            std::unique_ptr<windows_emulator> emu{};
            handle first_handle{};
            handle second_handle{};
        };
    }

    // Windows queues pointer input to the thread that owns the window the input is routed to.
    TEST_F(PointerRoutingTest, InputRoutedToAnotherThreadsChildIsQueuedToThatThread)
    {
        auto& owner = this->thread(this->first_handle);
        auto& child_owner = this->thread(this->second_handle);
        const auto root = this->create_window(owner.id, 0, ws_visible, 0, 0, 200, 200);
        const auto child = this->create_window(child_owner.id, root, ws_visible | ws_child, 10, 10, 50, 50);

        this->send_pointer(root, WM_LBUTTONDOWN, 20, 20, MK_LBUTTON);

        EXPECT_TRUE(owner.message_queue.empty());
        ASSERT_FALSE(child_owner.message_queue.empty());
        EXPECT_EQ(child, child_owner.message_queue.back().window);
        EXPECT_EQ(WM_LBUTTONDOWN, child_owner.message_queue.back().message);
    }

    // The shell's windows belong to explorer, not to a thread of the process: input captured by one is queued
    // nowhere, while the polled button state still follows the hardware.
    TEST_F(PointerRoutingTest, InputCapturedByAWindowWithoutAThreadIsNotQueued)
    {
        auto& owner = this->thread(this->first_handle);
        const auto root = this->create_window(owner.id, 0, ws_visible, 0, 0, 200, 200);
        const auto shell = this->create_window(0, 0, ws_visible, 0, 160, 200, 40);
        this->emu->process.set_mouse_capture(shell);

        this->send_pointer(root, WM_LBUTTONDOWN, 20, 170, MK_LBUTTON);

        EXPECT_TRUE(owner.message_queue.empty());
        EXPECT_TRUE(this->thread(this->second_handle).message_queue.empty());
        EXPECT_EQ(0x80, this->emu->process.key_state[VK_LBUTTON] & 0x80);

        this->send_pointer(root, WM_LBUTTONUP, 20, 170);

        EXPECT_TRUE(owner.message_queue.empty());
        EXPECT_EQ(0, this->emu->process.key_state[VK_LBUTTON] & 0x80);
    }

    TEST_F(PointerRoutingTest, MotionOverAWindowIsQueuedToItsThread)
    {
        auto& owner = this->thread(this->first_handle);
        const auto root = this->create_window(owner.id, 0, ws_visible, 0, 0, 200, 200);

        this->send_pointer(root, WM_MOUSEMOVE, 30, 40);

        ASSERT_FALSE(owner.message_queue.empty());
        EXPECT_EQ(root, owner.message_queue.back().window);
        EXPECT_EQ(WM_MOUSEMOVE, owner.message_queue.back().message);
    }

    TEST_F(PointerRoutingTest, OnlyAButtonPressMovesTheForeground)
    {
        const auto first = this->create_window(this->thread(this->first_handle).id, 0, ws_visible, 0, 0, 200, 200);
        const auto second = this->create_window(this->thread(this->second_handle).id, 0, ws_visible, 300, 0, 200, 200);
        const auto foreground = [&] { return this->emu->process.user_handles.get_server_info().read().foregroundWindow; };

        this->send_pointer(first, WM_LBUTTONDOWN, 20, 20, MK_LBUTTON);
        this->send_pointer(first, WM_LBUTTONUP, 20, 20);
        ASSERT_EQ(first, foreground());

        this->send_pointer(second, WM_MOUSEMOVE, 20, 20);
        this->send_pointer(second, WM_LBUTTONUP, 20, 20);
        EXPECT_EQ(first, foreground());

        this->send_pointer(second, WM_RBUTTONDOWN, 20, 20, MK_RBUTTON);
        EXPECT_EQ(second, foreground());
    }
}
