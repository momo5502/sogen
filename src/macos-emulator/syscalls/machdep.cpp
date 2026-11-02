#include "macos_syscalls.hpp"

namespace sogen
{
    void machdep_thread_fast_set_cthread_self(const macos_syscall_context& c)
    {
        c.emu.set_segment_base(x86_register::gs, get_macos_syscall_argument(c.emu, 0));
        write_trap_result(c, 0);
    }
}
