#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>

#include <array>
#include <optional>
#include <string>
#include <cstddef>
#include <cstdint>

namespace sogen
{
    class macos_emulator;

    namespace macos_errno
    {
        constexpr uint64_t MACOS_EPERM = 1;
        constexpr uint64_t MACOS_ENOENT = 2;
        constexpr uint64_t MACOS_ESRCH = 3;
        constexpr uint64_t MACOS_EBADF = 9;
        constexpr uint64_t MACOS_ENOMEM = 12;
        constexpr uint64_t MACOS_EACCES = 13;
        constexpr uint64_t MACOS_EFAULT = 14;
        constexpr uint64_t MACOS_ENODEV = 19;
        constexpr uint64_t MACOS_EINVAL = 22;
        constexpr uint64_t MACOS_ENOTTY = 25;
        constexpr uint64_t MACOS_ENOTSOCK = 38;
        constexpr uint64_t MACOS_EAFNOSUPPORT = 47;
        constexpr uint64_t MACOS_ENOTCONN = 57;
        constexpr uint64_t MACOS_ENOSPC = 28;
        constexpr uint64_t MACOS_EROFS = 30;
        constexpr uint64_t MACOS_ENAMETOOLONG = 63;
    }

    namespace macos_kern_return
    {
        constexpr uint64_t KERN_SUCCESS = 0;
        constexpr uint64_t KERN_INVALID_ADDRESS = 1;
        constexpr uint64_t KERN_NO_SPACE = 3;
        constexpr uint64_t KERN_INVALID_ARGUMENT = 4;
        constexpr uint64_t KERN_FAILURE = 5;
        constexpr uint64_t KERN_INVALID_NAME = 15;
    }

    // Not 1: dyld and libSystem take launchd-only paths for pid 1.
    constexpr uint32_t MACOS_PROCESS_ID = 100;

    // Distinguishes successive processes that reuse a pid; libxpc rejects an audit token whose version is zero.
    constexpr uint32_t MACOS_PROCESS_ID_VERSION = 1;

    // The identity of a standard first user account on macOS (staff group).
    constexpr uint32_t USER_ID = 501;
    constexpr uint32_t GROUP_ID = 20;

    // The wall-clock time the guest sees, fixed so runs are deterministic.
    constexpr int64_t MACOS_EPOCH_SECONDS = 1700000000;

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

    inline std::optional<std::string> read_guest_string(x86_64_emulator& emu, const uint64_t address, const size_t max_length = 1024)
    {
        std::string value{};
        for (size_t i = 0; i < max_length; ++i)
        {
            char character{};
            if (!emu.try_read_memory(address + i, &character, sizeof(character)))
            {
                return std::nullopt;
            }

            if (character == '\0')
            {
                return value;
            }

            value.push_back(character);
        }

        return std::nullopt;
    }

    // BSD syscalls report failure through the carry flag, with the positive errno in rax.
    inline void write_macos_syscall_result(const macos_syscall_context& c, const uint64_t value, const bool failed)
    {
        constexpr uint64_t carry_flag = 1;
        const auto rflags = c.emu.reg(x86_register::rflags);

        c.emu.reg(x86_register::rax, value);
        c.emu.reg(x86_register::rflags, failed ? (rflags | carry_flag) : (rflags & ~carry_flag));
    }

    // Mach traps and machine-dependent calls return their value in rax and leave the carry flag alone.
    inline void write_trap_result(const macos_syscall_context& c, const uint64_t value)
    {
        c.emu.reg(x86_register::rax, value);
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
