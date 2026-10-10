#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"
#include "../macos_memory_protection.hpp"

#include <address_utils.hpp>

#include <algorithm>

namespace sogen
{
    void mach_vm_deallocate_trap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 1);
        const auto size = get_macos_syscall_argument(c.emu, 2);

        if (size != 0)
        {
            c.emu_ref.memory.release_memory(address, static_cast<size_t>(size));
        }

        write_trap_result(c, macos_kern_return::KERN_SUCCESS);
    }

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

    void mach_vm_map_trap(const macos_syscall_context& c)
    {
        constexpr uint64_t vm_flags_anywhere = 0x1;

        const auto address_pointer = get_macos_syscall_argument(c.emu, 1);
        const auto size = page_align_up(get_macos_syscall_argument(c.emu, 2));
        const auto mask = get_macos_syscall_argument(c.emu, 3);
        const auto flags = get_macos_syscall_argument(c.emu, 4);
        const auto protection = get_macos_syscall_argument(c.emu, 5);

        uint64_t address{};
        if (size == 0 || !c.emu.try_read_memory(address_pointer, &address, sizeof(address)))
        {
            write_trap_result(c, macos_kern_return::KERN_INVALID_ARGUMENT);
            return;
        }

        auto& memory = c.emu_ref.memory;
        if (flags & vm_flags_anywhere)
        {
            const auto alignment = std::max<uint64_t>(mask + 1, 0x1000);
            const auto search_base = memory.find_free_allocation_base(static_cast<size_t>(size + alignment - 0x1000), address);
            address = search_base ? align_up(search_base, alignment) : 0;
        }

        if (address == 0 || !memory.allocate_memory(address, static_cast<size_t>(size), to_memory_permission(protection)))
        {
            write_trap_result(c, macos_kern_return::KERN_NO_SPACE);
            return;
        }

        c.emu.write_memory(address_pointer, &address, sizeof(address));
        write_trap_result(c, macos_kern_return::KERN_SUCCESS);
    }

    void mach_port_construct_trap(const macos_syscall_context& c)
    {
        const auto name_pointer = get_macos_syscall_argument(c.emu, 3);
        const auto name = c.emu_ref.allocate_mach_port_name();

        if (!c.emu.try_write_memory(name_pointer, &name, sizeof(name)))
        {
            write_trap_result(c, macos_kern_return::KERN_INVALID_ARGUMENT);
            return;
        }

        write_trap_result(c, macos_kern_return::KERN_SUCCESS);
    }

    void mach_reply_port(const macos_syscall_context& c)
    {
        write_trap_result(c, c.emu_ref.allocate_mach_port_name());
    }

    void mach_task_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, MACOS_TASK_PORT_NAME);
    }

    void mach_host_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, MACOS_HOST_PORT_NAME);
    }

    // mach_msg2 packs the header into register pairs: low 32 bits / high 32 bits of rdx (bits, send size),
    // r10 (remote port, local port) and r8 (voucher, message id). Messages are not modelled yet, so the
    // header is logged and emulation stops.
    void mach_msg2_trap(const macos_syscall_context& c)
    {
        const auto options = get_macos_syscall_argument(c.emu, 1);
        const auto bits_and_size = get_macos_syscall_argument(c.emu, 2);
        const auto ports = get_macos_syscall_argument(c.emu, 3);
        const auto voucher_and_id = get_macos_syscall_argument(c.emu, 4);

        c.emu_ref.log.error("Unhandled mach_msg2: options 0x%llx, bits 0x%x, send size %u, remote 0x%x, local 0x%x, id %u\n",
                            static_cast<unsigned long long>(options), static_cast<uint32_t>(bits_and_size),
                            static_cast<uint32_t>(bits_and_size >> 32), static_cast<uint32_t>(ports), static_cast<uint32_t>(ports >> 32),
                            static_cast<uint32_t>(voucher_and_id >> 32));
        c.emu_ref.stop();
    }
}
