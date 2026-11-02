#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

namespace sogen
{
    void sys_sigprocmask(const macos_syscall_context& c)
    {
        constexpr uint64_t sig_block = 1;
        constexpr uint64_t sig_unblock = 2;
        constexpr uint64_t sig_setmask = 3;

        const auto how = get_macos_syscall_argument(c.emu, 0);
        const auto set_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto old_set_pointer = get_macos_syscall_argument(c.emu, 2);

        auto& mask = c.emu_ref.signal_mask;
        if (old_set_pointer != 0 && !c.emu.try_write_memory(old_set_pointer, &mask, sizeof(mask)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (set_pointer != 0)
        {
            uint32_t set{};
            if (!c.emu.try_read_memory(set_pointer, &set, sizeof(set)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            switch (how)
            {
            case sig_block:
                mask |= set;
                break;
            case sig_unblock:
                mask &= ~set;
                break;
            case sig_setmask:
                mask = set;
                break;
            default:
                write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
                return;
            }
        }

        write_macos_syscall_success(c, 0);
    }
}
