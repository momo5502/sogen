#include "emulation_test_utils.hpp"
#include <cpu_context.hpp>

namespace sogen::test
{
    // The sample's main thread as its process setup leaves it, at LdrInitializeThunk with its initial CONTEXT.
    TEST(ThreadStartFlagsTest, ThreadsStartWithInterruptsEnabled)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();

        EXPECT_EQ(cpu_context::eflags_interrupt, cpu.reg<uint32_t>(x86_register::eflags) & cpu_context::eflags_interrupt);
        EXPECT_EQ(cpu_context::eflags_interrupt, emu.memory.read_memory<CONTEXT64>(cpu.reg(x86_register::rcx)).EFlags);
    }
}
