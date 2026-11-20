#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint8_t, 1> nop{0x90};
        constexpr uint64_t trap_flag = 0x100; // RFLAGS.TF
        constexpr uint64_t page_size = 0x1000;
    }

    // A one-instruction run whose instruction's execution hook stops ends at the hook's stop, before the instruction. The
    // run's own step is armed before the instruction's hooks run, so the stop must not wait for it: waiting re-entered the
    // instruction, its hook ran and stopped again, and start(1) never returned.
    TEST(OwnStepStopTest, AHookStopEndsAOneInstructionRunBeforeItsInstruction)
    {
        auto backend = create_x86_64_emulator_from_environment();
        auto& cpu = *backend;
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu{std::move(backend),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             {},
                             std::move(interfaces)};
        emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
        emu.vcpu(0).active_thread = &thread;

        const auto hooked = emu.memory.allocate_memory(page_size, memory_permission::all);
        emu.memory.write_memory(hooked, nop.data(), nop.size());

        size_t hook_runs = 0;
        auto* stopping = emu.emu().hook_memory_execution(hooked, [&](cpu_interface& hook_cpu, uint64_t) {
            // A run that re-enters the instruction ends here: with the stop still requested it re-entered for good.
            if (++hook_runs > 1)
            {
                throw std::runtime_error("the execution hook ran again in the same run");
            }

            hook_cpu.stop();
        });

        cpu.reg(x86_register::rip, hooked);
        EXPECT_NO_THROW(cpu.start(1));
        EXPECT_EQ(1u, hook_runs) << "the run ends at the hook's stop";
        EXPECT_EQ(hooked, cpu.reg(x86_register::rip)) << "the hook's stop comes before the instruction";
        EXPECT_EQ(0u, cpu.reg(x86_register::rflags) & trap_flag);

        emu.emu().delete_hook(stopping);
    }
}
