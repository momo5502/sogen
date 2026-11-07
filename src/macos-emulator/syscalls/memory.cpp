#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <posix_memory_syscalls.hpp>

#include <optional>

namespace sogen
{
    void sys_mmap(const macos_syscall_context& c)
    {
        constexpr uint64_t map_fixed = 0x10;
        constexpr uint64_t map_anonymous = 0x1000;

        const auto flags = get_macos_syscall_argument(c.emu, 3);
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 4));
        const posix_map_request request{
            .address = get_macos_syscall_argument(c.emu, 0),
            .length = get_macos_syscall_argument(c.emu, 1),
            .protection = get_macos_syscall_argument(c.emu, 2),
            .placement = (flags & map_fixed) ? posix_map_placement::fixed : posix_map_placement::hint,
            .fd = (flags & map_anonymous) ? std::nullopt : std::optional{fd},
            .offset = get_macos_syscall_argument(c.emu, 5),
        };

        write_macos_syscall_result(c, posix_mmap(c.emu_ref.memory, c.emu_ref.fds, request, posix_errno::einval));
    }

    void sys_munmap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 0);
        const auto length = get_macos_syscall_argument(c.emu, 1);
        write_macos_syscall_result(c, posix_munmap(c.emu_ref.memory, address, length));
    }

    void sys_mprotect(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 0);
        const auto length = get_macos_syscall_argument(c.emu, 1);
        const auto protection = get_macos_syscall_argument(c.emu, 2);
        write_macos_syscall_result(c, posix_mprotect(c.emu_ref.memory, address, length, protection, posix_errno::einval));
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
