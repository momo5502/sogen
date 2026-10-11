#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"
#include "../macos_memory_protection.hpp"
#include "../mig/macos_mig.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <vector>

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

    namespace
    {
        // With VM_FLAGS_ANYWHERE, mach_vm_map treats the incoming address as a placement hint and falls back to the
        // whole map, while mach_vm_allocate ignores it.
        void allocate_virtual_memory(const macos_syscall_context& c, const uint64_t address_pointer, const uint64_t requested_size,
                                     const uint64_t mask, const uint64_t flags, const memory_permission permissions, const bool honor_hint)
        {
            constexpr uint64_t vm_flags_anywhere = 0x1;

            const auto size = page_align_up(requested_size);

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
                const auto search_size = static_cast<size_t>(size + alignment - 0x1000);
                auto search_base = honor_hint ? memory.find_free_allocation_base(search_size, address) : 0;
                if (search_base == 0)
                {
                    search_base = memory.find_free_allocation_base(search_size);
                }

                address = search_base ? align_up(search_base, alignment) : 0;
            }

            c.emu_ref.log.log("  allocate 0x%llx bytes, flags 0x%llx, mask 0x%llx -> 0x%llx\n", static_cast<unsigned long long>(size),
                              static_cast<unsigned long long>(flags), static_cast<unsigned long long>(mask),
                              static_cast<unsigned long long>(address));

            if (address == 0 || !memory.allocate_memory(address, static_cast<size_t>(size), permissions))
            {
                write_trap_result(c, macos_kern_return::KERN_NO_SPACE);
                return;
            }

            c.emu.write_memory(address_pointer, &address, sizeof(address));
            write_trap_result(c, macos_kern_return::KERN_SUCCESS);
        }
    }

    void mach_vm_allocate_trap(const macos_syscall_context& c)
    {
        allocate_virtual_memory(c, get_macos_syscall_argument(c.emu, 1), get_macos_syscall_argument(c.emu, 2), 0,
                                get_macos_syscall_argument(c.emu, 3), memory_permission::read_write, false);
    }

    void mach_vm_map_trap(const macos_syscall_context& c)
    {
        allocate_virtual_memory(c, get_macos_syscall_argument(c.emu, 1), get_macos_syscall_argument(c.emu, 2),
                                get_macos_syscall_argument(c.emu, 3), get_macos_syscall_argument(c.emu, 4),
                                to_memory_permission(get_macos_syscall_argument(c.emu, 5)), true);
    }

    void mach_port_deallocate_trap(const macos_syscall_context& c)
    {
        const auto name = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 1));
        write_trap_result(c, c.emu_ref.mach_ports.find(name) != nullptr ? macos_kern_return::KERN_SUCCESS
                                                                        : macos_kern_return::KERN_INVALID_NAME);
    }

    void mach_port_mod_refs_trap(const macos_syscall_context& c)
    {
        mach_port_deallocate_trap(c);
    }

    void mach_port_construct_trap(const macos_syscall_context& c)
    {
        const auto name_pointer = get_macos_syscall_argument(c.emu, 3);
        const auto name = c.emu_ref.mach_ports.allocate_receive_right();

        if (!c.emu.try_write_memory(name_pointer, &name, sizeof(name)))
        {
            write_trap_result(c, macos_kern_return::KERN_INVALID_ARGUMENT);
            return;
        }

        write_trap_result(c, macos_kern_return::KERN_SUCCESS);
    }

    void mach_reply_port(const macos_syscall_context& c)
    {
        write_trap_result(c, c.emu_ref.mach_ports.allocate_receive_right());
    }

    void mach_thread_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, macos_mach_ports::THREAD_PORT_NAME);
    }

    void mach_task_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, macos_mach_ports::TASK_PORT_NAME);
    }

    void mach_host_self_trap(const macos_syscall_context& c)
    {
        write_trap_result(c, macos_mach_ports::HOST_PORT_NAME);
    }

    // mach_msg2 takes eight arguments: the first six in registers, the last two (receive size and priority,
    // then timeout) on the user stack above the return address. The send size is the high half of rdx.
    void mach_msg2_trap(const macos_syscall_context& c)
    {
        constexpr uint64_t mach64_send_msg = 0x1;
        constexpr uint64_t mach64_rcv_msg = 0x2;
        constexpr uint64_t mach_msg_success = 0;
        constexpr uint64_t mach_send_invalid_dest = 0x10000003;
        constexpr uint64_t mach_send_msg_too_small = 0x10000008;
        constexpr uint64_t mach_rcv_too_large = 0x10004004;
        constexpr uint32_t max_message_size = 0x10000;

        const auto data = get_macos_syscall_argument(c.emu, 0);
        const auto options = get_macos_syscall_argument(c.emu, 1);
        const auto send_size = static_cast<uint32_t>(get_macos_syscall_argument(c.emu, 2) >> 32);

        if ((options & mach64_send_msg) == 0)
        {
            c.emu_ref.log.error("Unhandled mach_msg2 without a send (options 0x%llx)\n", static_cast<unsigned long long>(options));
            c.emu_ref.stop();
            return;
        }

        if (send_size < sizeof(mach_msg_header) || send_size > max_message_size)
        {
            write_trap_result(c, mach_send_msg_too_small);
            return;
        }

        std::vector<std::byte> request(send_size);
        c.emu.read_memory(data, request.data(), request.size());
        const auto header = read_mig_field<mach_msg_header>(request, 0);

        const auto* destination = c.emu_ref.mach_ports.find(header.remote_port);
        c.emu_ref.log.log("  message %d to port 0x%x\n", header.id, header.remote_port);
        if (destination == nullptr || destination->object == mach_port_object::dead)
        {
            write_trap_result(c, mach_send_invalid_dest);
            return;
        }

        const auto routine = destination->object == mach_port_object::user ? nullptr : find_mig_routine(destination->object, header.id);
        if (routine == nullptr)
        {
            c.emu_ref.log.error("Unhandled Mach message: id %d to port 0x%x (object %u)\n", header.id, header.remote_port,
                                static_cast<unsigned>(destination->object));
            c.emu_ref.stop();
            return;
        }

        const auto reply = routine(c, request);
        if (reply.empty())
        {
            c.emu_ref.stop();
            return;
        }

        if (options & mach64_rcv_msg)
        {
            uint64_t receive_size_and_priority{};
            c.emu.read_memory(c.emu.reg(x86_register::rsp) + sizeof(uint64_t), &receive_size_and_priority,
                              sizeof(receive_size_and_priority));

            if (reply.size() > static_cast<uint32_t>(receive_size_and_priority))
            {
                write_trap_result(c, mach_rcv_too_large);
                return;
            }

            c.emu.write_memory(data, reply.data(), reply.size());
        }

        write_trap_result(c, mach_msg_success);
    }
}
