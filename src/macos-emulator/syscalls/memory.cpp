#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <address_utils.hpp>

namespace sogen
{
    namespace
    {
        constexpr uint64_t PROT_READ = 0x1;
        constexpr uint64_t PROT_WRITE = 0x2;
        constexpr uint64_t PROT_EXEC = 0x4;

        memory_permission to_memory_permission(const uint64_t protection)
        {
            auto permission = memory_permission::none;

            if (protection & PROT_READ)
            {
                permission |= memory_permission::read;
            }

            if (protection & PROT_WRITE)
            {
                permission |= memory_permission::write;
            }

            if (protection & PROT_EXEC)
            {
                permission |= memory_permission::exec;
            }

            return permission;
        }
    }

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
}
