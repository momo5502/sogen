#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};
        constexpr std::array<uint8_t, 3> rdtscp{0x0F, 0x01, 0xF9};
        constexpr std::array<uint8_t, 2> cpuid{0x0F, 0xA2};
        constexpr uint8_t nop = 0x90;
        constexpr uint64_t trap_flag = 0x100; // RFLAGS.TF
    }

    // emulator.start(1) runs one instruction, which the WHP backend does with a trap flag step of its own. The hypervisor
    // performs RDTSC, CPUID and RDTSCP in exits, which raise no single-step trap: the run still ends right after each, with
    // the trap flag as the guest had it.
    TEST(PerformedInstructionStepTest, AOneInstructionRunEndsAfterAnInstructionTheHypervisorPerforms)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        std::vector<uint8_t> code_bytes(rdtsc.begin(), rdtsc.end());
        code_bytes.insert(code_bytes.end(), cpuid.begin(), cpuid.end());
        code_bytes.insert(code_bytes.end(), rdtscp.begin(), rdtscp.end());
        code_bytes.push_back(nop);
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, code_bytes.data(), code_bytes.size());
        cpu.reg(x86_register::rip, code);

        emu.start(1);
        EXPECT_EQ(code + rdtsc.size(), cpu.read_instruction_pointer());
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);

        emu.start(1);
        EXPECT_EQ(code + rdtsc.size() + cpuid.size(), cpu.read_instruction_pointer());
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);

        emu.start(1);
        EXPECT_EQ(code + rdtsc.size() + cpuid.size() + rdtscp.size(), cpu.read_instruction_pointer());
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);
    }
}
