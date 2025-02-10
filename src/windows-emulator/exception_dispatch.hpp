#pragma once

#include <x64_emulator.hpp>

#include <platform/traits.hpp>
#include <platform/primitives.hpp>

struct process_context;

void dispatch_exception(x64_emulator& emu, const process_context& proc, DWORD status,
                        const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters);
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, DWORD>)
void dispatch_exception(x64_emulator& emu, const process_context& proc, const T status,
                        const std::vector<EmulatorTraits<Emu64>::ULONG_PTR>& parameters)
{
    dispatch_exception(emu, proc, static_cast<DWORD>(status), parameters);
}

void dispatch_access_violation(x64_emulator& emu, const process_context& proc, uint64_t address,
                               memory_operation operation);
void dispatch_illegal_instruction_violation(x64_emulator& emu, const process_context& proc);
void dispatch_integer_division_by_zero(x64_emulator& emu, const process_context& proc);
void dispatch_single_step(x64_emulator& emu, const process_context& proc);
