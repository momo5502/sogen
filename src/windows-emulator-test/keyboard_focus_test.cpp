#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::test
{
    namespace
    {
        class KeyboardFocusTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                const auto base = emu->memory.allocate_memory(0x10000, memory_permission::read_write);
                emu->process.base_allocator = emulator_allocator{emu->memory, base, 0x10000};
                emu->process.user_handles.setup(false);
                const auto thread_handle =
                    emu->process.threads.store(emulator_thread{emu->memory, emu->process, 0, 0, 0x100000, 0, 8, true});
                thread = emu->process.threads.get(thread_handle);
                emu->vcpu(0).active_thread = thread;
                auto [parent_handle, parent_window] = emu->process.windows.create(emu->memory);
                parent = parent_handle.bits;
                parent_window.handle = parent;
                parent_window.thread_id = thread->id;
                auto [child_handle, child_window] = emu->process.windows.create(emu->memory);
                child = child_handle.bits;
                child_window.handle = child;
                child_window.thread_id = thread->id;
                child_window.parent_handle = parent;
                thread->set_focus_window(child);
            }

            std::unique_ptr<windows_emulator> emu{};
            emulator_thread* thread{};
            hwnd parent{};
            hwnd child{};
        };
    }

    TEST_F(KeyboardFocusTest, HostKeyboardAndCharacterMessagesReachTheFocusedChild)
    {
        emu->handle_ui_event({.window = parent, .message = WM_KEYDOWN, .wParam = 'A'});
        emu->handle_ui_event({.window = parent, .message = WM_CHAR, .wParam = 'a'});
        emu->handle_ui_event({.window = parent, .message = WM_KEYUP, .wParam = 'A'});

        ASSERT_EQ(3u, thread->message_queue.size());
        for (const auto& message : thread->message_queue)
        {
            EXPECT_EQ(child, message.window);
        }
        EXPECT_EQ(WM_KEYDOWN, thread->message_queue[0].message);
        EXPECT_EQ(WM_CHAR, thread->message_queue[1].message);
        EXPECT_EQ('a', thread->message_queue[1].wParam);
        EXPECT_EQ(WM_KEYUP, thread->message_queue[2].message);
    }

    TEST_F(KeyboardFocusTest, InputFallsBackToItsWindowWhenNothingHasFocus)
    {
        thread->set_focus_window(0);
        emu->handle_ui_event({.window = parent, .message = WM_CHAR, .wParam = 'a'});
        ASSERT_EQ(1u, thread->message_queue.size());
        EXPECT_EQ(parent, thread->message_queue.front().window);
    }

}
