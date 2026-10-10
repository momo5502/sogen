#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sogen
{
    class macos_emulator;

    namespace macos_errno
    {
        constexpr uint64_t MACOS_EBADF = 9;
        constexpr uint64_t MACOS_ENOMEM = 12;
        constexpr uint64_t MACOS_EFAULT = 14;
        constexpr uint64_t MACOS_EINVAL = 22;
    }

    struct macos_syscall_context
    {
        macos_emulator& emu_ref;
        x86_64_emulator& emu;
    };

    using macos_syscall_handler = void (*)(const macos_syscall_context&);

    inline uint64_t get_macos_syscall_argument(x86_64_emulator& emu, const size_t index)
    {
        constexpr std::array argument_registers{x86_register::rdi, x86_register::rsi, x86_register::rdx,
                                                x86_register::r10, x86_register::r8,  x86_register::r9};
        return emu.reg(argument_registers.at(index));
    }

    // BSD syscalls report failure through the carry flag, with the positive errno in rax.
    inline void write_macos_syscall_result(const macos_syscall_context& c, const uint64_t value, const bool failed)
    {
        constexpr uint64_t carry_flag = 1;
        const auto rflags = c.emu.reg(x86_register::rflags);

        c.emu.reg(x86_register::rax, value);
        c.emu.reg(x86_register::rflags, failed ? (rflags | carry_flag) : (rflags & ~carry_flag));
    }

    inline void write_macos_syscall_success(const macos_syscall_context& c, const uint64_t value)
    {
        write_macos_syscall_result(c, value, false);
    }

    inline void write_macos_syscall_error(const macos_syscall_context& c, const uint64_t error)
    {
        write_macos_syscall_result(c, error, true);
    }
}
