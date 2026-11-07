#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t ws_child = 0x40000000;
        constexpr uint32_t ws_visible = 0x10000000;
        constexpr uint32_t bs_groupbox = 0x00000007;

        class WindowHitTestingTest : public testing::Test
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

            hwnd create_window(const uint32_t thread_id, const hwnd parent, const std::u16string_view class_name, const uint32_t style,
                               const int32_t x, const int32_t y, const int32_t width, const int32_t height)
            {
                auto [window_handle, win] = this->emu->process.windows.create(this->emu->memory);
                win.thread_id = thread_id;
                win.handle = window_handle.bits;
                win.parent_handle = parent;
                win.class_name = class_name;
                win.style = style;
                win.x = x;
                win.y = y;
                win.width = width;
                win.height = height;
                return win.handle;
            }

            hwnd click_at(const hwnd root, const int32_t x, const int32_t y)
            {
                auto* thread = this->emu->process.threads.get(this->thread_handle);
                EXPECT_NE(nullptr, thread);
                if (!thread)
                {
                    return 0;
                }

                const auto l_param = static_cast<uint16_t>(x) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16);
                this->emu->handle_ui_event({.window = root, .message = WM_LBUTTONDOWN, .lParam = l_param});
                EXPECT_FALSE(thread->message_queue.empty());
                if (thread->message_queue.empty())
                {
                    return 0;
                }
                EXPECT_EQ(WM_LBUTTONDOWN, thread->message_queue.back().message);
                return thread->message_queue.back().window;
            }

            std::unique_ptr<windows_emulator> emu{};
            handle thread_handle{};
        };
    }

    TEST_F(WindowHitTestingTest, ChildInsideGroupBoxReceivesMouseInput)
    {
        auto* thread = this->emu->process.threads.get(this->thread_handle);
        ASSERT_NE(nullptr, thread);

        const auto root = this->create_window(thread->id, 0, u"Root", ws_visible, 0, 0, 200, 200);
        const auto group_box = this->create_window(thread->id, root, u"Button", ws_visible | ws_child | bs_groupbox, 10, 10, 100, 100);
        const auto button = this->create_window(thread->id, group_box, u"Button", ws_visible | ws_child, 20, 20, 40, 30);

        EXPECT_EQ(button, this->click_at(root, 35, 35));
    }

    TEST_F(WindowHitTestingTest, BareGroupBoxPassesMouseInputToUnderlyingWindow)
    {
        auto* thread = this->emu->process.threads.get(this->thread_handle);
        ASSERT_NE(nullptr, thread);

        const auto root = this->create_window(thread->id, 0, u"Root", ws_visible, 0, 0, 200, 200);
        const auto underlying = this->create_window(thread->id, root, u"Button", ws_visible | ws_child, 10, 10, 100, 100);
        this->create_window(thread->id, root, u"Button", ws_visible | ws_child | bs_groupbox, 10, 10, 100, 100);

        EXPECT_EQ(underlying, this->click_at(root, 35, 35));
    }

    TEST_F(WindowHitTestingTest, StaticWithoutNotifyPassesMouseInputToUnderlyingWindow)
    {
        auto* thread = this->emu->process.threads.get(this->thread_handle);
        ASSERT_NE(nullptr, thread);

        const auto root = this->create_window(thread->id, 0, u"Root", ws_visible, 0, 0, 200, 200);
        const auto underlying = this->create_window(thread->id, root, u"Button", ws_visible | ws_child, 10, 10, 100, 100);
        this->create_window(thread->id, root, u"Static", ws_visible | ws_child, 10, 10, 100, 100);

        EXPECT_EQ(underlying, this->click_at(root, 35, 35));
    }
}
