#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

namespace sogen
{
    void sys_exit(const macos_syscall_context& c)
    {
        c.emu_ref.exit_status = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        c.emu_ref.stop();
    }
}
