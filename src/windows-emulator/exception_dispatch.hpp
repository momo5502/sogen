#pragma once

#include <arch_emulator.hpp>

#include <platform/traits.hpp>
#include <platform/primitives.hpp>

struct process_context;

void dispatch_exception(x86_64_emulator& emu, const process_context& proc, DWORD status,
                        const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters);
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, DWORD>)
void dispatch_exception(x86_64_emulator& emu, const process_context& proc, const T status,
                        const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
{
    dispatch_exception(emu, proc, static_cast<DWORD>(status), parameters);
}

void dispatch_access_violation(x86_64_emulator& emu, const process_context& proc, uint64_t address, memory_operation operation);
void dispatch_guard_page_violation(x86_64_emulator& emu, const process_context& proc, uint64_t address, memory_operation operation);
void dispatch_illegal_instruction_violation(x86_64_emulator& emu, const process_context& proc);
void dispatch_integer_division_by_zero(x86_64_emulator& emu, const process_context& proc);
void dispatch_single_step(x86_64_emulator& emu, const process_context& proc);
void dispatch_breakpoint(x86_64_emulator& emu, const process_context& proc);
