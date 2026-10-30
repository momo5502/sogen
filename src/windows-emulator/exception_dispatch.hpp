#pragma once

#include <arch_emulator.hpp>

#include <platform/traits.hpp>
#include <platform/primitives.hpp>

//
// DebugService Control Types
//
#define BREAKPOINT_BREAK          0
#define BREAKPOINT_PRINT          1
#define BREAKPOINT_PROMPT         2
#define BREAKPOINT_LOAD_SYMBOLS   3
#define BREAKPOINT_UNLOAD_SYMBOLS 4
#define BREAKPOINT_COMMAND_STRING 5

namespace sogen
{

    class windows_emulator;
    struct vcpu_context;

    void dispatch_exception(windows_emulator& win_emu, vcpu_context& vcpu, DWORD status,
                            const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters);

    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, DWORD>)
    void dispatch_exception(windows_emulator& win_emu, vcpu_context& vcpu, const T status,
                            const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
    {
        dispatch_exception(win_emu, vcpu, static_cast<DWORD>(status), parameters);
    }

    bool dispatch_debug_exception(windows_emulator& win_emu, CONTEXT64& ctx, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record);

    // The 32-bit view WOW64 keeps of a thread running x86 code.
    WOW64_CONTEXT make_wow64_context(const CONTEXT64& ctx);

    // An attached debugger sees an exception before the guest's handlers do. True when the debugger handled it; the
    // thread then resumes from whatever context the debugger left.
    bool report_exception_to_debugger(windows_emulator& win_emu, vcpu_context& vcpu,
                                      const EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>& record, bool first_chance);

    // Hands an exception the guest raised itself (NtRaiseException) to its handlers through KiUserExceptionDispatcher.
    void dispatch_exception_record(windows_emulator& win_emu, vcpu_context& vcpu, EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> record,
                                   CONTEXT64 ctx);
    void dispatch_access_violation(windows_emulator& win_emu, vcpu_context& vcpu, uint64_t address, memory_operation operation);
    void dispatch_guard_page_violation(windows_emulator& win_emu, vcpu_context& vcpu, uint64_t address, memory_operation operation);
    void dispatch_illegal_instruction_violation(windows_emulator& win_emu, vcpu_context& vcpu);
    void dispatch_integer_division_by_zero(windows_emulator& win_emu, vcpu_context& vcpu);
    void dispatch_single_step(windows_emulator& win_emu, vcpu_context& vcpu);
    void dispatch_breakpoint(windows_emulator& win_emu, vcpu_context& vcpu);

} // namespace sogen
