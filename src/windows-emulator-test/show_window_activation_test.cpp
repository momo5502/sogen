#include "std_include.hpp"

namespace sogen::syscalls
{
    BOOL handle_NtUserShowWindow(const syscall_context& c, hwnd hwnd, LONG cmd_show);
    BOOL completion_NtUserShowWindow(const syscall_context& c, hwnd hwnd, LONG cmd_show);
    BOOL handle_NtUserSetForegroundWindow(const syscall_context& c, hwnd hwnd);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view k_plain_class = u"ShowWindowActivationTestClass";
        constexpr std::u16string_view k_save_bits_class = u"ShowWindowActivationTestSaveBitsClass";

        class ShowWindowActivationTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                // IncVisWindows treats the right to take the foreground differently from 10.0.26100.5074 on, whatever the
                // emulation root is.
                this->emu.version.set_windows_build_number(WINDOWS_VERSION::WINDOWS_10_22H2);

                EMU_WNDCLASSEX save_bits_class{};
                save_bits_class.style = CS_SAVEBITS;
                this->emu.process.classes.insert_or_assign(
                    std::u16string{k_save_bits_class},
                    process_context::class_entry{0, save_bits_class, {}, 0, std::u16string{k_save_bits_class}});
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            emulator_thread& thread()
            {
                return *this->emu.vcpu(0).active_thread;
            }

            hwnd create_window(const std::u16string_view class_name = k_plain_class, const uint32_t ex_style = 0,
                               const uint32_t style = WS_POPUP)
            {
                auto [handle, win] = this->emu.process.windows.create(this->emu.memory);
                win.handle = handle.bits;
                win.parent_handle = this->emu.process.default_desktop_window_handle.bits;
                win.thread_id = this->thread().id;
                win.class_name = class_name;
                win.style = style;
                win.ex_style = ex_style;
                win.width = 50;
                win.height = 50;
                return win.handle;
            }

            // Calls ShowWindow and runs the messages it sends, as if each window procedure returned 0 right away.
            void show_window(const hwnd target, const LONG cmd_show)
            {
                auto call = this->context();
                (void)syscalls::handle_NtUserShowWindow(call, target, cmd_show);
                bool callback_pending = call.run_callback;
                while (callback_pending)
                {
                    auto& callbacks = this->thread().callback_stack;
                    auto frame = std::move(callbacks.back());
                    callbacks.pop_back();
                    frame.restore_registers(this->emu.vcpu(0).cpu);

                    auto completion = this->context();
                    completion.is_callback_completion = true;
                    completion.current_completion_state = frame.state.get();
                    (void)syscalls::completion_NtUserShowWindow(completion, target, cmd_show);
                    callback_pending = completion.run_callback;
                }
            }

            DWORD min_max_style(const hwnd target)
            {
                return this->emu.process.windows.get(target)->guest.read().dwStyle & (WS_MINIMIZE | WS_MAXIMIZE);
            }

            hwnd foreground_window()
            {
                return this->emu.process.user_handles.get_server_info().read().foregroundWindow;
            }

            void activate_another_application(const hwnd foreground)
            {
                this->emu.handle_ui_event({.window = foreground, .message = WM_ACTIVATE, .wParam = WA_INACTIVE});
            }

            void click(const hwnd target)
            {
                this->emu.handle_ui_event({.window = target, .message = WM_LBUTTONDOWN, .wParam = MK_LBUTTON});
            }

            BOOL set_foreground_window(const hwnd target)
            {
                auto call = this->context();
                return syscalls::handle_NtUserSetForegroundWindow(call, target);
            }

            windows_emulator emu = create_sample_emulator();
        };
    }

    TEST_F(ShowWindowActivationTest, TheFirstWindowShownTakesTheForeground)
    {
        const auto first = this->create_window();
        const auto second = this->create_window();

        this->show_window(first, SW_SHOW);
        EXPECT_EQ(first, this->foreground_window());
        EXPECT_EQ(first, this->thread().active_window);

        this->show_window(second, SW_SHOWNORMAL);
        EXPECT_EQ(second, this->foreground_window());
        EXPECT_EQ(second, this->thread().active_window);
    }

    TEST_F(ShowWindowActivationTest, ShowingWithoutActivationKeepsTheRightToTakeTheForeground)
    {
        const auto not_activated = this->create_window();
        const auto not_activated_either = this->create_window();
        const auto activated = this->create_window();

        this->show_window(not_activated, SW_SHOWNA);
        this->show_window(not_activated_either, SW_SHOWNOACTIVATE);
        EXPECT_EQ(0u, this->thread().active_window);

        this->show_window(activated, SW_SHOWDEFAULT);
        EXPECT_EQ(activated, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowShownWhileAnotherApplicationIsInTheForegroundOnlyBecomesActive)
    {
        const auto first = this->create_window();
        const auto second = this->create_window();
        this->show_window(first, SW_SHOW);
        this->activate_another_application(first);

        this->show_window(second, SW_SHOW);
        EXPECT_EQ(second, this->thread().active_window);
        EXPECT_NE(second, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowActivatedBeforeItIsVisibleLeavesTheRightToTakeTheForeground)
    {
        const auto save_bits = this->create_window(k_save_bits_class);
        const auto next = this->create_window();
        this->show_window(save_bits, SW_SHOW);
        this->activate_another_application(save_bits);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowActivatedBeforeItIsVisibleLeavesTheRightOn26100Revision1742)
    {
        this->emu.version.set_windows_build_number(WINDOWS_VERSION::WINDOWS_11_24H2);
        this->emu.version.set_windows_update_build_revision(1742);
        const auto save_bits = this->create_window(k_save_bits_class);
        const auto next = this->create_window();
        this->show_window(save_bits, SW_SHOW);
        this->activate_another_application(save_bits);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowActivatedBeforeItIsVisibleLosesTheRightFrom26100Revision5074)
    {
        this->emu.version.set_windows_build_number(WINDOWS_VERSION::WINDOWS_11_24H2);
        this->emu.version.set_windows_update_build_revision(5074);
        const auto save_bits = this->create_window(k_save_bits_class);
        const auto next = this->create_window();
        this->show_window(save_bits, SW_SHOW);
        this->activate_another_application(save_bits);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, ANoActivateWindowBecomesActiveButSpendsTheRightToTakeTheForeground)
    {
        const auto no_activate = this->create_window(k_plain_class, WS_EX_NOACTIVATE);
        const auto next = this->create_window();

        this->show_window(no_activate, SW_SHOW);
        EXPECT_EQ(no_activate, this->thread().active_window);
        EXPECT_NE(no_activate, this->foreground_window());

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowShownMinimizedOrMaximizedTakesTheForeground)
    {
        const auto minimized = this->create_window();
        const auto maximized = this->create_window();

        this->show_window(minimized, SW_SHOWMINIMIZED);
        EXPECT_EQ(minimized, this->foreground_window());

        this->show_window(maximized, SW_SHOWMAXIMIZED);
        EXPECT_EQ(maximized, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, RestoringAHiddenMinimizedWindowTakesTheForeground)
    {
        const auto minimized = this->create_window(k_plain_class, 0, WS_POPUP | WS_MINIMIZE);

        this->show_window(minimized, SW_RESTORE);
        EXPECT_EQ(minimized, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AMinimizedWindowShownMinimizedStaysInactive)
    {
        const auto minimized = this->create_window(k_plain_class, 0, WS_POPUP | WS_MINIMIZE);

        this->show_window(minimized, SW_SHOWMINIMIZED);
        EXPECT_EQ(0u, this->thread().active_window);
        EXPECT_NE(minimized, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, RestoringAMinimizedWindowLeavesNoRightToTakeTheForeground)
    {
        const auto restored = this->create_window(k_plain_class, 0, WS_POPUP | WS_MINIMIZE);
        const auto next = this->create_window();
        this->show_window(restored, SW_RESTORE);
        this->activate_another_application(restored);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AWindowShownMinimizedLeavesTheRightToTakeTheForeground)
    {
        const auto minimized = this->create_window();
        const auto next = this->create_window();
        this->show_window(minimized, SW_SHOWMINIMIZED);
        this->activate_another_application(minimized);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AVisibleWindowThatIsMinimizedNoLongerCounts)
    {
        const auto minimized_later = this->create_window();
        const auto shown_minimized = this->create_window();
        const auto next = this->create_window();
        this->show_window(minimized_later, SW_SHOWNA);
        this->show_window(minimized_later, SW_SHOWMINNOACTIVE);
        this->show_window(shown_minimized, SW_SHOWMINIMIZED);
        this->activate_another_application(shown_minimized);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AVisibleWindowThatIsRestoredCountsAgain)
    {
        const auto restored = this->create_window();
        const auto shown_minimized = this->create_window();
        const auto next = this->create_window();
        this->show_window(restored, SW_SHOWMINNOACTIVE);
        this->show_window(restored, SW_SHOWNOACTIVATE);
        this->show_window(shown_minimized, SW_SHOWMINIMIZED);
        this->activate_another_application(shown_minimized);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AMaximizedWindowRestoredWithoutActivationIsActivatedBeforeItIsVisible)
    {
        const auto save_bits = this->create_window(k_save_bits_class, 0, WS_POPUP | WS_MAXIMIZE);
        const auto next = this->create_window();
        this->show_window(save_bits, SW_SHOWNOACTIVATE);
        this->show_window(save_bits, SW_HIDE);
        this->show_window(save_bits, SW_SHOWNORMAL);
        this->activate_another_application(save_bits);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, RestoringAWindowMinimizedWhileMaximizedMaximizesItAgain)
    {
        const auto restored = this->create_window(k_plain_class, 0, WS_POPUP | WS_MAXIMIZE);
        const auto shown_normal = this->create_window(k_plain_class, 0, WS_POPUP | WS_MAXIMIZE);
        this->show_window(restored, SW_SHOWMINNOACTIVE);
        this->show_window(shown_normal, SW_SHOWMINNOACTIVE);
        EXPECT_EQ(static_cast<DWORD>(WS_MINIMIZE), this->min_max_style(restored));

        this->show_window(restored, SW_RESTORE);
        this->show_window(shown_normal, SW_SHOWNORMAL);
        EXPECT_EQ(static_cast<DWORD>(WS_MAXIMIZE), this->min_max_style(restored));
        EXPECT_EQ(0u, this->min_max_style(shown_normal));
    }

    TEST_F(ShowWindowActivationTest, RestoringTheOnlyVisibleWindowFromMinimizedTakesTheForegroundAndKeepsTheRight)
    {
        const auto restored = this->create_window();
        const auto next = this->create_window();
        this->show_window(restored, SW_SHOWMINNOACTIVE);
        EXPECT_EQ(0u, this->thread().active_window);

        this->show_window(restored, SW_RESTORE);
        EXPECT_EQ(restored, this->thread().active_window);
        EXPECT_EQ(restored, this->foreground_window());
        this->activate_another_application(restored);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, RestoringAWindowBesideAnotherVisibleOneActivatesItAndSpendsTheRight)
    {
        const auto restored = this->create_window();
        const auto visible = this->create_window();
        const auto next = this->create_window();
        this->show_window(restored, SW_SHOWMINNOACTIVE);
        this->show_window(visible, SW_SHOWNA);

        this->show_window(restored, SW_RESTORE);
        EXPECT_EQ(restored, this->foreground_window());
        this->activate_another_application(restored);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, MaximizingAVisibleWindowActivatesItAndSpendsTheRight)
    {
        const auto maximized = this->create_window();
        const auto next = this->create_window();
        this->show_window(maximized, SW_SHOWNA);

        this->show_window(maximized, SW_SHOWMAXIMIZED);
        EXPECT_EQ(maximized, this->thread().active_window);
        EXPECT_EQ(maximized, this->foreground_window());
        this->activate_another_application(maximized);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, ACommandThatChangesNothingLeavesAVisibleWindowInactive)
    {
        const auto shown = this->create_window();
        this->show_window(shown, SW_SHOWNA);

        this->show_window(shown, SW_SHOW);
        this->show_window(shown, SW_RESTORE);
        EXPECT_EQ(0u, this->thread().active_window);
        EXPECT_NE(shown, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, RestoringTheForegroundWindowSpendsTheRightFrom26100Revision5074)
    {
        this->emu.version.set_windows_build_number(WINDOWS_VERSION::WINDOWS_11_24H2);
        this->emu.version.set_windows_update_build_revision(5074);
        const auto restored = this->create_window();
        const auto next = this->create_window();
        this->show_window(restored, SW_SHOWMINIMIZED);
        this->show_window(restored, SW_RESTORE);
        this->activate_another_application(restored);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, AClickSpendsTheRightToTakeTheForeground)
    {
        const auto clicked = this->create_window();
        const auto next = this->create_window();
        this->show_window(clicked, SW_SHOWNA);
        this->click(clicked);
        EXPECT_EQ(clicked, this->foreground_window());
        this->activate_another_application(clicked);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, SetForegroundWindowSpendsTheRightToTakeTheForeground)
    {
        const auto first = this->create_window();
        const auto next = this->create_window();
        this->show_window(first, SW_SHOWNA);
        EXPECT_TRUE(this->set_foreground_window(first));
        this->activate_another_application(first);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->thread().active_window);
        EXPECT_NE(next, this->foreground_window());
    }

    TEST_F(ShowWindowActivationTest, ReactivatingTheForegroundThreadKeepsTheRightToTakeTheForeground)
    {
        const auto minimized = this->create_window();
        const auto other = this->create_window();
        const auto next = this->create_window();
        this->show_window(minimized, SW_SHOWMINIMIZED);
        this->show_window(other, SW_SHOWNA);

        this->click(minimized);
        EXPECT_TRUE(this->set_foreground_window(other));
        this->activate_another_application(other);

        this->show_window(next, SW_SHOW);
        EXPECT_EQ(next, this->foreground_window());
    }
}
