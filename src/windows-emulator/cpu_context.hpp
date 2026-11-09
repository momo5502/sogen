#pragma once
#include "arch_emulator.hpp"
#include <platform/arm64_context.hpp>

namespace sogen
{

    namespace cpu_context
    {
        // DR7_LEGAL: what a user-mode context may set in DR7 (L0-L3, LE, GE and the breakpoint conditions).
        constexpr uint32_t dr7_legal = 0xFFFF0355;
        // DR7_ACTIVE: the enables of DR7 (L0-L3, LE, GE) that make the debug registers part of a thread's state.
        constexpr uint64_t dr7_active = 0x355;

        // KGDT64_R3_CMCODE, KGDT64_R3_DATA and KGDT64_R3_CODE with RPL 3.
        constexpr uint16_t wow64_code_selector = 0x23;
        constexpr uint16_t user_data_selector = 0x2B;
        constexpr uint16_t user_code_selector = 0x33;

        // EFLAGS_USER_SANITIZE: what a user-mode context may set in EFlags (CF, PF, AF, ZF, SF, TF, DF, OF, RF, ID).
        constexpr uint32_t eflags_user_legal = 0x210DD5;
        // EFLAGS_IF_MASK: user mode always runs with interrupts enabled.
        constexpr uint32_t eflags_interrupt = 0x200;
        // EFLAGS_TF_MASK: the trap flag, which KiDispatchException clears before the dispatcher runs.
        constexpr uint32_t eflags_trap = 0x100;
        // Bit 1 of EFlags, which the processor always holds set; a trap frame holds it clear.
        constexpr uint32_t eflags_always_one = 0x2;

        // PspCreateUserContext's FltSave.ControlWord and MxCsr for a new thread, and PspWow64InitThreadGuestx86's for a
        // 32-bit one: every x87 and SSE exception masked, rounding to nearest and the x87's 53-bit precision.
        constexpr uint16_t initial_x87_control = 0x27F;
        constexpr uint32_t initial_mxcsr = 0x1F80;

        // What KxContextToKframes keeps of a user context's x87 control word: the exception masks, the precision and
        // rounding controls and the infinity bit.
        constexpr uint16_t x87_control_legal = 0x1F3F;
        // Bit 6 of the x87 control word, which the processor holds set.
        constexpr uint16_t x87_control_always_one = 0x40;
        // MXCSR_MASK as FXSAVE stores it on the processors sogen runs on (QEMU's, and Intel's since DAZ): every MXCSR
        // bit. KiInitializeKernel takes it for KiMxCsrMask, the MXCSR bits a context may set.
        constexpr uint32_t mxcsr_legal = 0xFFFF;

        // Whether a user context reaches the trap frame through KiVerifyContextRecord, which gives a native process's
        // context the 64-bit code selector, as NtContinue's and NtSetContextThread's do, or straight through
        // KxContextToKframes, as NtRaiseException's does: KiRaiseException verifies it only for a thread with a CET user
        // shadow stack, which sogen's threads never have.
        enum class context_verification
        {
            verified,
            unverified,
        };

        // Puts a user context's control registers as KxContextToKframes keeps them in the trap frame, where
        // KeContextFromKframes reads them back for an exception's handlers; bit 1 of EFlags stays clear there.
        void sanitize_control(CONTEXT64& context, bool wow64_process, context_verification verification);

        void save(x86_64_cpu& emu, CONTEXT64& context);
        void restore(x86_64_cpu& emu, const CONTEXT64& context, bool wow64_process,
                     context_verification verification = context_verification::verified);
        void save(arm64_cpu& emu, arm64_context& context);
        void restore(arm64_cpu& emu, const arm64_context& context);
    }

} // namespace sogen
