#pragma once

#include <arch_emulator.hpp>
#include <memory_interface.hpp>
#include <memory_permission.hpp>
#include <address_utils.hpp>
#include <serialization.hpp>
#include <platform/elf.hpp>

#include <guest/guest_memory_object.hpp>
#include <utils/time.hpp>

// --------------------------------------------------------------------------
// emulator_pointer
// --------------------------------------------------------------------------

namespace sogen
{

    using emulator_pointer = uint64_t;

    template <typename T>
    using emulator_object = guest_object<T>;

    using emulator_allocator = guest_allocator;

    template <typename Element>
    std::basic_string<Element> read_string(memory_interface& mem, const uint64_t address, const std::optional<size_t> size = {})
    {
        return read_guest_string<Element>(mem, address, size);
    }

    // --------------------------------------------------------------------------
    // Linux syscall argument accessor (System V convention)
    // --------------------------------------------------------------------------

    inline uint64_t get_linux_syscall_argument(x86_64_emulator& emu, const size_t index)
    {
        switch (index)
        {
        case 0:
            return emu.reg(x86_register::rdi);
        case 1:
            return emu.reg(x86_register::rsi);
        case 2:
            return emu.reg(x86_register::rdx);
        case 3:
            return emu.reg(x86_register::r10);
        case 4:
            return emu.reg(x86_register::r8);
        case 5:
            return emu.reg(x86_register::r9);
        default:
            throw std::runtime_error("Linux syscalls have at most 6 register arguments");
        }
    }

    // --------------------------------------------------------------------------
    // ELF segment permission helper
    // --------------------------------------------------------------------------

    inline memory_permission elf_segment_to_permission(uint32_t p_flags)
    {
        memory_permission perm = memory_permission::none;

        if (p_flags & elf::PF_R)
        {
            perm = perm | memory_permission::read;
        }
        if (p_flags & elf::PF_W)
        {
            perm = perm | memory_permission::write;
        }
        if (p_flags & elf::PF_X)
        {
            perm = perm | memory_permission::exec;
        }

        return perm;
    }

} // namespace sogen
