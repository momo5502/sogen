#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        // What PspCreateUserContext and PspWow64InitThreadGuestx86 give a new thread.
        constexpr uint16_t windows_initial_x87_control = 0x27F;
        constexpr uint32_t windows_initial_mxcsr = 0x1F80;
    }

    TEST(ThreadStartFpuTest, ThreadsStartWithTheControlWordsWindowsGivesThem)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto ldr_initialize_thunk_context = emu.memory.read_memory<CONTEXT64>(cpu.reg(x86_register::rcx));

        EXPECT_EQ(windows_initial_x87_control, cpu.reg<uint16_t>(x86_register::fpcw));
        EXPECT_EQ(windows_initial_mxcsr, cpu.reg<uint32_t>(x86_register::mxcsr));
        EXPECT_EQ(windows_initial_x87_control, ldr_initialize_thunk_context.FltSave.ControlWord);
    }

    TEST(ThreadStartFpuTest, Wow64ThreadsStartWithTheControlWordsWindowsGivesThem)
    {
        auto emu = create_empty_emulator();
        emu.process.is_wow64_process = true;
        const emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, false};
        const auto context = thread.wow64_cpu_reserved->read().Context;

        XMM_SAVE_AREA32 extended{};
        memcpy(&extended, context.ExtendedRegisters, sizeof(extended));
        EXPECT_EQ(windows_initial_x87_control, context.FloatSave.ControlWord);
        EXPECT_EQ(windows_initial_x87_control, extended.ControlWord);
        EXPECT_EQ(windows_initial_mxcsr, extended.MxCsr);
    }
}
