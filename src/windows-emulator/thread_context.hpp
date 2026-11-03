#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sogen
{
    class windows_emulator;
    class emulator_thread;
    struct vcpu_context;

    // A thread's register context in the guest's CONTEXT layout, read and written as GetThreadContext and
    // SetThreadContext do. Must run on the thread that owns vcpu, with the kernel lock held.
    std::vector<uint8_t> capture_thread_context(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread);
    bool apply_thread_context(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread, std::span<const uint8_t> bytes);

    // The WOW64_CONTEXT that ThreadWow64Context reads and writes. A thread stopped in x86 code has it in its registers,
    // like the kernel's trap frame; otherwise WOW64 keeps it in WOW64_CPURESERVED.
    std::vector<uint8_t> capture_wow64_thread_context(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread);
    bool apply_wow64_thread_context(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread, std::span<const uint8_t> bytes);
}
