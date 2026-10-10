#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

namespace sogen
{
    namespace
    {
        // Not 1: dyld and libSystem take launchd-only paths for pid 1.
        constexpr uint64_t PROCESS_ID = 100;
        constexpr uint64_t MAIN_THREAD_ID = 0x1000;
    }

    void sys_exit(const macos_syscall_context& c)
    {
        c.emu_ref.exit_status = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        c.emu_ref.stop();
    }

    void sys_thread_selfid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, MAIN_THREAD_ID);
    }

    void sys_getpid(const macos_syscall_context& c)
    {
        write_macos_syscall_success(c, PROCESS_ID);
    }
}
