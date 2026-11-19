#include "std_include.hpp"

namespace sogen::syscalls
{
    uint64_t handle_NtUserCallHwndLock(const syscall_context& c, hwnd hwnd, uint32_t routine);
    BOOL handle_NtUserSetForegroundWindow(const syscall_context& c, hwnd hwnd);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t set_foreground_window_routine = 112;

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
            }

            hwnd create_window(const uint32_t style = 0)
            {
                auto [handle, window] = this->emu.process.windows.create(this->emu.memory);
                window.handle = handle.bits;
                window.thread_id = this->context.thread().id;
                window.style = style;
                return handle.bits;
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
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
}
