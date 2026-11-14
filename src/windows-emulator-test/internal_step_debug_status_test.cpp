#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t unreachable_breakpoint = 0x10000;
        constexpr uint64_t dr7_l0 = 0x1;
        // L0 with R/W0 = write and LEN0 = 4 bytes.
        constexpr uint64_t dr7_l0_write_dword = 0x000D0001;
        constexpr uint64_t dr6_b0 = 0x1;
        constexpr uint64_t dr6_status_bits = 0xE00F; // B0-B3, BD, BS, BT
        // mov [rax], rcx; nop
        constexpr std::array<uint8_t, 4> write_then_nop{0x48, 0x89, 0x08, 0x90};
        constexpr size_t step_limit = 4;
    }

    // emulator.start(1) runs one instruction, which the WHP backend does with a trap flag step of its own.
    TEST(InternalStepDebugStatusTest, AnEmulatorStepLeavesTheDebugStatusAsItWas)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        cpu.reg(x86_register::dr0, unreachable_breakpoint);
        cpu.reg(x86_register::dr7, dr7_l0);
        const auto dr6 = cpu.reg(x86_register::dr6);

        emu.start(1);
        EXPECT_EQ(dr6, cpu.reg(x86_register::dr6));
    }

    // A breakpoint flag DR6 already holds, as a CONTEXT a breakpoint's handler continued with leaves it, is no hit of the
    // step: no debug exception reaches the guest, and DR6 keeps the flag.
    TEST(InternalStepDebugStatusTest, ABreakpointFlagDr6AlreadyHeldIsNoHitOfTheStep)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        cpu.reg(x86_register::dr0, unreachable_breakpoint);
        cpu.reg(x86_register::dr7, dr7_l0);
        cpu.reg(x86_register::dr6, cpu.reg(x86_register::dr6) | dr6_b0);
        const auto dr6 = cpu.reg(x86_register::dr6);

        emu.start(1);
        EXPECT_NE(emu.process.ki_user_exception_dispatcher, cpu.read_instruction_pointer());
        EXPECT_EQ(dr6, cpu.reg(x86_register::dr6));
    }

    TEST(InternalStepDebugStatusTest, ABreakpointTheStepMeetsStillReachesTheGuest)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        const auto data = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        emu.memory.write_memory(code, write_then_nop.data(), write_then_nop.size());
        cpu.reg(x86_register::rip, code);
        cpu.reg(x86_register::rax, data);
        cpu.reg(x86_register::dr0, data);
        cpu.reg(x86_register::dr7, dr7_l0_write_dword);

        // Unicorn runs an instruction that meets a data breakpoint again on its own to finish its access: a second step.
        for (size_t i = 0; i < step_limit && cpu.read_instruction_pointer() != emu.process.ki_user_exception_dispatcher; ++i)
        {
            emu.start(1);
        }

        EXPECT_EQ(emu.process.ki_user_exception_dispatcher, cpu.read_instruction_pointer());
        EXPECT_EQ(dr6_b0, emu.memory.read_memory<CONTEXT64>(cpu.reg(x86_register::rsp)).Dr6 & dr6_status_bits);
    }
}
