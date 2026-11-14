#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        // KiUserApcDispatcher's unwind data: UWOP_ALLOC_LARGE 0x530, then UWOP_PUSH_MACHFRAME.
        constexpr uint64_t apc_machine_frame_offset = 0x530;
        constexpr uint64_t stack_alignment = 0x10;
        constexpr uint64_t apc_routine = 0x1000;
    }

    // KiInitializeUserApc leaves a MACHINE_FRAME with the interrupted RIP and RSP right under the interrupted stack, at
    // (Rsp - 0x28) & ~0xF, the frame KiUserApcDispatcher starts on 0x530 below it.
    TEST(UserApcFrameTest, TheApcFrameHoldsWhereTheThreadWasInterrupted)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);

        pending_apc apc{};
        apc.apc_routine = apc_routine;
        vcpu.active_thread->pending_apcs.push_back(apc);
        vcpu.active_thread->apc_alertable = true;
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));

        auto& cpu = vcpu.x64_cpu();
        ASSERT_EQ(emu.process.ki_user_apc_dispatcher, cpu.read_instruction_pointer());

        const auto rsp = cpu.read_stack_pointer();
        const auto interrupted = emu.memory.read_memory<CONTEXT64>(rsp);
        const auto machine_frame = emu.memory.read_memory<MACHINE_FRAME>(rsp + apc_machine_frame_offset);
        EXPECT_EQ((interrupted.Rsp - sizeof(MACHINE_FRAME)) & ~(stack_alignment - 1), rsp + apc_machine_frame_offset);
        EXPECT_EQ(interrupted.Rip, machine_frame.Rip);
        EXPECT_EQ(interrupted.Rsp, machine_frame.Rsp);
    }
}
