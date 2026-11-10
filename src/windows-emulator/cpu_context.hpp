#pragma once
#include "arch_emulator.hpp"
#include <platform/arm64_context.hpp>

namespace sogen
{

    namespace cpu_context
    {
        // DR7_LEGAL: what a user-mode context may set in DR7 (L0-L3, LE, GE and the breakpoint conditions).
        constexpr uint32_t dr7_legal = 0xFFFF0355;

        void save(x86_64_cpu& emu, CONTEXT64& context);
        void restore(x86_64_cpu& emu, const CONTEXT64& context);
        void save(arm64_cpu& emu, arm64_context& context);
        void restore(arm64_cpu& emu, const arm64_context& context);
    }

} // namespace sogen
