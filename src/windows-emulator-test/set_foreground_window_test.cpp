#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    BOOL handle_NtUserCallHwndLock(const syscall_context& c, hwnd hwnd, uint32_t routine);
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
            }

            windows_emulator emu = create_empty_emulator();
            syscall_context context{
                .win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
        };
    }

    TEST_F(SetForegroundWindowTest, MakesTheWindowTheOneClientSideGetForegroundWindowReports)
    {
        auto [handle, window] = this->emu.process.windows.create(this->emu.memory);
        window.handle = handle.bits;

        EXPECT_TRUE(syscalls::handle_NtUserCallHwndLock(this->context, handle.bits, set_foreground_window_routine));
        EXPECT_EQ(handle.bits, this->emu.process.foreground_window);
        EXPECT_EQ(handle.bits, this->emu.process.user_handles.get_server_info().read().foregroundWindow);
    }
}
