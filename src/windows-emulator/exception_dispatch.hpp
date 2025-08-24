#pragma once

#include <arch_emulator.hpp>

#include <platform/traits.hpp>
#include <platform/primitives.hpp>

class windows_emulator;

void dispatch_exception(windows_emulator& win_emu, DWORD status, const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters);
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, DWORD>)
void dispatch_exception(windows_emulator& win_emu, const T status, const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
{
    dispatch_exception(win_emu, static_cast<DWORD>(status), parameters);
}

void dispatch_access_violation(windows_emulator& win_emu, uint64_t address, memory_operation operation);
void dispatch_guard_page_violation(windows_emulator& win_emu, uint64_t address, memory_operation operation);
void dispatch_illegal_instruction_violation(windows_emulator& win_emu);
void dispatch_integer_division_by_zero(windows_emulator& win_emu);
void dispatch_single_step(windows_emulator& win_emu);
void dispatch_breakpoint(windows_emulator& win_emu);
