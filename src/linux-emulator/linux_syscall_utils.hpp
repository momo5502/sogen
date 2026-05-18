#pragma once

#include "std_include.hpp"
#include "linux_emulator_utils.hpp"
#include "linux_process_context.hpp"

class linux_emulator;

struct linux_syscall_context
{
    linux_emulator& emu_ref;
    x86_64_emulator& emu;
    linux_process_context& proc;
};

using linux_syscall_handler = void (*)(const linux_syscall_context&);

inline void write_linux_syscall_result(const linux_syscall_context& c, const int64_t result)
{
    c.emu.reg(x86_register::rax, static_cast<uint64_t>(result));
}

template <typename T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
T resolve_linux_argument(x86_64_emulator& emu, const size_t index)
{
    return static_cast<T>(get_linux_syscall_argument(emu, index));
}

template <typename T>
T resolve_linux_indexed_argument(x86_64_emulator& emu, size_t& index)
{
    return resolve_linux_argument<T>(emu, index++);
}
