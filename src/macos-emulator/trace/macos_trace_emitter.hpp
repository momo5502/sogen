#pragma once

#include <cstdint>
#include <string_view>

namespace sogen
{
    class macos_emulator;
    struct macos_syscall_context;

    void emit_bsd_syscall_trace(macos_emulator& emu, const macos_syscall_context& context, uint32_t number, std::string_view name);
    void emit_mach_trap_trace(macos_emulator& emu, const macos_syscall_context& context, uint32_t index, std::string_view name);
    void emit_syscall_error_trace(macos_emulator& emu, std::string_view name);
}
