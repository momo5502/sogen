#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};
        constexpr std::array<uint8_t, 3> rdtscp{0x0F, 0x01, 0xF9};
        constexpr std::array<uint8_t, 2> cpuid{0x0F, 0xA2};
        constexpr std::array<uint8_t, 2> syscall_instruction{0x0F, 0x05};
        constexpr uint8_t nop = 0x90;
        constexpr uint64_t trap_flag = 0x100; // RFLAGS.TF
        // An ntdll system service stub: mov r10, rcx; mov eax, <id>; test; jne; syscall; ret; int 0x2e; ret.
        constexpr size_t service_stub_size = 0x20;
        constexpr size_t service_stub_step_limit = 8;
        constexpr size_t dispatcher_step_limit = 4;
        // KiUserExceptionDispatcher's stack holds the CONTEXT, then the exception record.
        constexpr uint64_t context_record_size = 0x4F0;
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

    // On WHP the single-step trap after SYSCALL lands in the backend's syscall page, before the backend makes the call.
    TEST(PerformedInstructionStepTest, AOneInstructionRunEndsAfterASyscall)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        ASSERT_NE(emu.mod_manager.ntdll, nullptr);
        const auto stub = emu.mod_manager.ntdll->find_export("NtFlushProcessWriteBuffers");
        ASSERT_NE(stub, 0u);

        std::array<uint8_t, service_stub_size> stub_code{};
        emu.memory.read_memory(stub, stub_code.data(), stub_code.size());
        const auto found = std::ranges::search(stub_code, syscall_instruction);
        ASSERT_FALSE(found.empty());
        const auto syscall_address = stub + static_cast<uint64_t>(found.begin() - stub_code.begin());

        auto& cpu = emu.vcpu(0).x64_cpu();
        cpu.reg(x86_register::rip, stub);
        for (size_t i = 0; i < service_stub_step_limit && cpu.read_instruction_pointer() != syscall_address; ++i)
        {
            emu.start(1);
        }
        ASSERT_EQ(syscall_address, cpu.read_instruction_pointer());

        emu.start(1);
        EXPECT_EQ(syscall_address + syscall_instruction.size(), cpu.read_instruction_pointer());
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);
        EXPECT_EQ(0u, cpu.reg(x86_register::r11) & trap_flag) << "SYSCALL saved the flags in R11";
    }

    // The guest's own trap flag raises its single-step trap right after CPUID, before the next instruction runs.
    TEST(PerformedInstructionStepTest, TheGuestsOwnStepOverCpuidTrapsRightAfterIt)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        std::vector<uint8_t> code_bytes(cpuid.begin(), cpuid.end());
        code_bytes.push_back(nop);
        code_bytes.push_back(nop);
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, code_bytes.data(), code_bytes.size());
        cpu.reg(x86_register::rip, code);
        cpu.reg(x86_register::rflags, cpu.reg(x86_register::rflags) | trap_flag);

        // Unicorn can take a second step to reach KiUserExceptionDispatcher.
        for (size_t i = 0; i < dispatcher_step_limit && cpu.read_instruction_pointer() != emu.process.ki_user_exception_dispatcher; ++i)
        {
            emu.start(1);
        }

        ASSERT_EQ(emu.process.ki_user_exception_dispatcher, cpu.read_instruction_pointer());
        const auto record =
            emu.memory.read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(cpu.reg(x86_register::rsp) + context_record_size);
        EXPECT_EQ(STATUS_SINGLE_STEP, static_cast<NTSTATUS>(record.ExceptionCode));
        EXPECT_EQ(code + cpuid.size(), record.ExceptionAddress);
    }
}
