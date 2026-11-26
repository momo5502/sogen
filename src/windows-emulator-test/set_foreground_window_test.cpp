#include "std_include.hpp"

namespace sogen::syscalls
{
    uint64_t handle_NtUserCallHwndLock(const syscall_context& c, hwnd hwnd, uint32_t routine);
    BOOL handle_NtUserSetForegroundWindow(const syscall_context& c, hwnd hwnd);
    BOOL handle_NtUserClipCursor(const syscall_context& c, emulator_pointer rect_ptr);
    BOOL handle_NtUserGetClipCursor(const syscall_context& c, emulator_pointer rect_ptr);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t set_foreground_window_routine = 112;
        constexpr uint32_t other_thread_id = 12;
        constexpr RECT cursor_clip{.left = 10, .top = 20, .right = 30, .bottom = 40};

        class cursor_clip_ui final : public ui_backend
        {
          public:
            void set_event_sink(event_sink /*sink*/) override
            {
            }

            void pump_events() override
            {
            }

            void set_cursor_clip(const hwnd /*window*/, const std::optional<ui_rect>& screen_rect, const bool /*relative*/) override
            {
                this->clip = screen_rect;
            }

            std::optional<ui_rect> clip{};
        };

        auto corners(const RECT& rect)
        {
            return std::tuple{rect.left, rect.top, rect.right, rect.bottom};
        }

        class SetForegroundWindowTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                // NtUserCallHwndLock routine numbers are pinned to Windows 10 19041-19045, whatever the emulation root is.
                this->emu.version.set_windows_build_number(19045);

                this->emu.process.user_handles.setup(false);
                const auto thread =
                    this->emu.process.threads.store(emulator_thread{this->emu.memory, this->emu.process, 0, 0, 0, 0, 8, true});
                this->emu.vcpu(0).active_thread = this->emu.process.threads.get(thread);
                this->rect_buffer =
                    this->emu.memory.allocate_memory(static_cast<size_t>(page_align_up(sizeof(RECT))), memory_permission::read_write);
            }

            hwnd create_window(const uint32_t style = 0)
            {
                auto [handle, window] = this->emu.process.windows.create(this->emu.memory);
                window.handle = handle.bits;
                window.thread_id = this->context.thread().id;
                window.style = style;
                return handle.bits;
            }

            hwnd create_window_of_another_thread()
            {
                this->emu.process.threads.store(emulator_thread{this->emu.memory, this->emu.process, 0, 0, 0, 0, other_thread_id, true});
                const auto window = this->create_window(WS_VISIBLE);
                this->emu.process.windows.get(window)->thread_id = other_thread_id;
                return window;
            }

            void clip_cursor(const RECT& rect)
            {
                this->emu.memory.write_memory(this->rect_buffer, rect);
                ASSERT_EQ(TRUE, syscalls::handle_NtUserClipCursor(this->context, this->rect_buffer));
            }

            RECT get_clip_cursor()
            {
                EXPECT_EQ(TRUE, syscalls::handle_NtUserGetClipCursor(this->context, this->rect_buffer));
                return this->emu.memory.read_memory<RECT>(this->rect_buffer);
            }

            emulator_interfaces recording_ui()
            {
                auto backend = std::make_unique<cursor_clip_ui>();
                this->ui = backend.get();
                emulator_interfaces interfaces{};
                interfaces.ui = std::move(backend);
                return interfaces;
            }

            cursor_clip_ui* ui{};
            windows_emulator emu = create_empty_emulator(this->recording_ui());
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            uint64_t rect_buffer{};
        };
    }

    TEST_F(SetForegroundWindowTest, MakesTheWindowTheOneClientSideGetForegroundWindowReports)
    {
        const auto window = this->create_window();

        EXPECT_TRUE(syscalls::handle_NtUserCallHwndLock(this->context, window, set_foreground_window_routine));
        EXPECT_EQ(window, this->emu.process.foreground_window);
        EXPECT_EQ(window, this->emu.process.user_handles.get_server_info().read().foregroundWindow);
    }

    TEST_F(SetForegroundWindowTest, DedicatedSyscallSwitchesForegroundAndFocus)
    {
        this->emu.version.set_windows_build_number(26100);
        const auto first = this->create_window(WS_VISIBLE);
        const auto second = this->create_window();

        for (const auto window : {first, second, first})
        {
            ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, window));
            EXPECT_EQ(window, this->emu.process.user_handles.get_server_info().read().foregroundWindow);
            EXPECT_EQ(window, this->context.thread().active_window);
            EXPECT_EQ(window, this->context.thread().focus_window);
        }
    }

    TEST_F(SetForegroundWindowTest, RejectedTargetsKeepThePreviousForeground)
    {
        this->emu.version.set_windows_build_number(26100);
        const auto window = this->create_window();
        const auto child = this->create_window(WS_CHILD | WS_VISIBLE);
        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, window));

        set_guest_last_error(this->context, 0x5a5a);
        EXPECT_EQ(FALSE, syscalls::handle_NtUserSetForegroundWindow(this->context, child));
        EXPECT_EQ(0x5a5au, this->context.thread().teb64->read().LastErrorValue);

        for (const auto bad : {hwnd{0}, hwnd{0x12345678}})
        {
            set_guest_last_error(this->context, 0x5a5a);
            EXPECT_EQ(FALSE, syscalls::handle_NtUserSetForegroundWindow(this->context, bad));
            EXPECT_EQ(1400u, this->context.thread().teb64->read().LastErrorValue);
        }
        EXPECT_EQ(window, this->emu.process.user_handles.get_server_info().read().foregroundWindow);
        EXPECT_EQ(window, this->context.thread().focus_window);
    }

    TEST_F(SetForegroundWindowTest, AnotherThreadsWindowTakingTheForegroundClearsTheCursorClip)
    {
        const auto unclipped = this->get_clip_cursor();
        const auto window = this->create_window(WS_VISIBLE);
        const auto other_window = this->create_window_of_another_thread();
        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, window));
        this->clip_cursor(cursor_clip);
        ASSERT_TRUE(this->ui->clip.has_value());

        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, other_window));

        EXPECT_EQ(corners(unclipped), corners(this->get_clip_cursor()));
        EXPECT_FALSE(this->ui->clip.has_value());
    }

    TEST_F(SetForegroundWindowTest, AWindowOfTheSameThreadTakingTheForegroundKeepsTheCursorClip)
    {
        const auto first = this->create_window(WS_VISIBLE);
        const auto second = this->create_window(WS_VISIBLE);
        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, first));
        this->clip_cursor(cursor_clip);

        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, second));

        EXPECT_EQ(corners(cursor_clip), corners(this->get_clip_cursor()));
        EXPECT_TRUE(this->ui->clip.has_value());
    }

    TEST_F(SetForegroundWindowTest, TheHostDeactivatingTheForegroundWindowClearsTheCursorClip)
    {
        const auto unclipped = this->get_clip_cursor();
        const auto window = this->create_window(WS_VISIBLE);
        ASSERT_EQ(TRUE, syscalls::handle_NtUserSetForegroundWindow(this->context, window));
        this->clip_cursor(cursor_clip);

        ui_event deactivation{};
        deactivation.window = window;
        deactivation.message = WM_ACTIVATE;
        deactivation.wParam = WA_INACTIVE;
        this->emu.handle_ui_event(deactivation);

        EXPECT_EQ(corners(unclipped), corners(this->get_clip_cursor()));
        EXPECT_FALSE(this->ui->clip.has_value());
    }
}
