#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"
#include "../macos_memory_protection.hpp"

#include <address_utils.hpp>

namespace sogen
{
    void mach_vm_protect_trap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);
        const auto set_maximum = get_macos_syscall_argument(c.emu, 3) != 0;
        const auto protection = get_macos_syscall_argument(c.emu, 4);

        if (set_maximum || size == 0)
        {
            write_trap_result(c, macos_kern_return::KERN_SUCCESS);
            return;
        }

        const auto start = page_align_down(address);
        const auto end = page_align_up(address + size);
        const auto protected_memory =
            c.emu_ref.memory.protect_memory(start, static_cast<size_t>(end - start), to_memory_permission(protection));

        write_trap_result(c, protected_memory ? macos_kern_return::KERN_SUCCESS : macos_kern_return::KERN_INVALID_ADDRESS);
    }

    void mach_reply_port(const macos_syscall_context& c)
    {
        write_trap_result(c, c.emu_ref.allocate_mach_port_name());
    }

    void mach_task_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, MACOS_TASK_PORT_NAME);
    }
}
