#pragma once
#include "arch_emulator.hpp"
#include <platform/arm64_context.hpp>

namespace sogen
{

    namespace cpu_context
    {
        // DR7_LEGAL: what a user-mode context may set in DR7 (L0-L3, LE, GE and the breakpoint conditions).
        constexpr uint32_t dr7_legal = 0xFFFF0355;

        // What KxContextToKframes keeps of a user context's x87 control word: the exception masks, the precision and
        // rounding controls and the infinity bit.
        constexpr uint16_t x87_control_legal = 0x1F3F;
        // Bit 6 of the x87 control word, which the processor holds set.
        constexpr uint16_t x87_control_always_one = 0x40;
        // MXCSR_MASK as FXSAVE stores it on the processors sogen runs on (QEMU's, and Intel's since DAZ): every MXCSR
        // bit. KiInitializeKernel takes it for KiMxCsrMask, the MXCSR bits a context may set.
        constexpr uint32_t mxcsr_legal = 0xFFFF;

        void save(x86_64_cpu& emu, CONTEXT64& context);
        void restore(x86_64_cpu& emu, const CONTEXT64& context);
        void save(arm64_cpu& emu, arm64_context& context);
        void restore(arm64_cpu& emu, const arm64_context& context);
    }

} // namespace sogen
