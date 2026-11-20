#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 1> nop{0x90};
        constexpr std::array<uint8_t, 2> rdtsc{0x0F, 0x31};                           // where each run stops
        constexpr std::array<uint8_t, 5> mov_ecx_imm32{0xB9, 0x78, 0x56, 0x34, 0x12}; // rdtsc leaves ECX alone
        constexpr uint64_t mov_ecx_value = 0x12345678;
        constexpr uint64_t trap_flag = 0x100; // RFLAGS.TF
        constexpr uint64_t page_size = 0x1000;
    }

    // An execution hook's step that a stop deferred (the hook ran, the stop came before the instruction) is for a run back
    // at the hooked instruction. Taken by the next run on the vCPU, another thread's at an instruction an execution hook
    // narrowed on another page, it threw "Nested WHP execution single-step state is not supported" there; that run drops it
    // instead.
    TEST(ExecutionHookStepTest, ADeferredStepIsOnlyForItsInstruction)
    {
        auto backend = create_x86_64_emulator_from_environment();
        auto& cpu = *backend;
        emulator_callbacks observers{};
        observers.on_rdtsc = [&] { cpu.stop(); };
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu{std::move(backend),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             std::move(observers), std::move(interfaces)};
        emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
        emu.vcpu(0).active_thread = &thread;

        const auto hooked = emu.memory.allocate_memory(0x1000, memory_permission::all);
        const auto other = emu.memory.allocate_memory(0x1000, memory_permission::all);
        for (const auto page : {hooked, other})
        {
            emu.memory.write_memory(page, nop.data(), nop.size());
            emu.memory.write_memory(page + nop.size(), rdtsc.data(), rdtsc.size());
        }

        auto* stopping = emu.emu().hook_memory_execution(hooked, [](cpu_interface& hook_cpu, uint64_t) { hook_cpu.stop(); });
        size_t other_runs = 0;
        auto* counting = emu.emu().hook_memory_execution(other, [&](cpu_interface&, uint64_t) { ++other_runs; });

        cpu.reg(x86_register::rip, hooked);
        cpu.start();
        EXPECT_EQ(hooked, cpu.reg(x86_register::rip)) << "the hook's stop comes before the instruction";

        cpu.reg(x86_register::rip, other); // the vCPU's next run is another thread's
        EXPECT_NO_THROW(cpu.start());
        EXPECT_EQ(other + nop.size() + rdtsc.size(), cpu.reg(x86_register::rip));
        EXPECT_EQ(1u, other_runs);
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);

        emu.emu().delete_hook(counting);
        emu.emu().delete_hook(stopping);
    }

    // An instruction that runs into an execution-hooked page faults on that page's start while RIP is still on the page
    // before, so its hook runs for the hooked page's address. The step a stop deferred is for that instruction and opens the
    // hooked page: the thread's next run gets past the instruction without running the hook again.
    TEST(ExecutionHookStepTest, ADeferredStepOpensThePageItsInstructionRunsInto)
    {
        if (get_x86_64_emulator_backend_from_environment() != backend_type::whp)
        {
            GTEST_SKIP() << "Only WHP runs an execution hook for an instruction's fetch from the hooked page";
        }

        auto backend = create_x86_64_emulator_from_environment();
        auto& cpu = *backend;
        emulator_callbacks observers{};
        observers.on_rdtsc = [&] { cpu.stop(); };
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu{std::move(backend),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             std::move(observers), std::move(interfaces)};
        emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
        emu.vcpu(0).active_thread = &thread;

        // The mov's opcode and first immediate byte end the first page; the rest of it and the rdtsc start the hooked one.
        constexpr uint64_t bytes_before_hooked_page = 2;
        const auto first = emu.memory.allocate_memory(2 * page_size, memory_permission::all);
        const auto hooked = first + page_size;
        const auto instruction = hooked - bytes_before_hooked_page;
        emu.memory.write_memory(instruction, mov_ecx_imm32.data(), mov_ecx_imm32.size());
        emu.memory.write_memory(instruction + mov_ecx_imm32.size(), rdtsc.data(), rdtsc.size());

        size_t hook_runs = 0;
        auto* stopping = emu.emu().hook_memory_execution(hooked, [&](cpu_interface& hook_cpu, uint64_t) {
            if (++hook_runs == 1) // only the first, so a run that never gets past the instruction cannot hang the test
            {
                hook_cpu.stop();
            }
        });

        cpu.reg(x86_register::rip, instruction);
        cpu.start();
        EXPECT_EQ(instruction, cpu.reg(x86_register::rip)) << "the hook's stop comes before the instruction";
        EXPECT_EQ(1u, hook_runs);

        cpu.start();
        EXPECT_EQ(instruction + mov_ecx_imm32.size() + rdtsc.size(), cpu.reg(x86_register::rip));
        EXPECT_EQ(mov_ecx_value, cpu.reg(x86_register::rcx));
        EXPECT_EQ(1u, hook_runs) << "the deferred step runs the instruction its hook already ran for";
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);

        emu.emu().delete_hook(stopping);
    }
}
