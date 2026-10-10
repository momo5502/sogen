#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include "../macos_memory_protection.hpp"

#include <address_utils.hpp>

namespace sogen
{
    void sys_mprotect(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 0);
        const auto length = static_cast<size_t>(get_macos_syscall_argument(c.emu, 1));
        const auto protection = get_macos_syscall_argument(c.emu, 2);

        if (length == 0 || page_align_down(address) != address)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        if (!c.emu_ref.memory.protect_memory(address, static_cast<size_t>(page_align_up(length)), to_memory_permission(protection)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_shared_region_check_np(const macos_syscall_context& c)
    {
        if (!c.emu_ref.shared_region)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto start_address_out = get_macos_syscall_argument(c.emu, 0);
        const auto base_address = c.emu_ref.shared_region->base_address();
        if (!c.emu.try_write_memory(start_address_out, &base_address, sizeof(base_address)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
