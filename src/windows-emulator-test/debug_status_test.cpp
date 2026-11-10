#include "emulation_test_utils.hpp"
#include <exception_dispatch.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t breakpoint = 0x10000;
        constexpr uint64_t dr7_l0 = 0x1;
        // DR6 after a single step: BS and the reserved bits the processor reads as 1.
        constexpr uint64_t stepped_dr6 = 0xFFFF4FF0;
        constexpr uint64_t dr6_status_bits = 0xE00F; // B0-B3, BD, BS, BT

        class DebugStatusTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                emulator_interfaces interfaces{};
                interfaces.ui = std::make_unique<null_ui_backend>();
                interfaces.audio = std::make_unique<null_audio_backend>();
                interfaces.console = std::make_unique<null_console_backend>();
                this->emu = std::make_unique<windows_emulator>(
                    create_x86_64_emulator_from_environment(),
                    emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                    emulator_callbacks{}, std::move(interfaces));
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                auto* active = this->emu->process.threads.get(thread);
                this->emu->vcpu(0).active_thread = active;
                this->cpu().reg(x86_register::rsp, active->stack_base + active->stack_size - 0x1000);
                // No GDT before process setup: a dispatch then reads no descriptor for CS and stays 64-bit.
                this->cpu().load_gdt(0, 0);
            }

            x86_64_cpu& cpu()
            {
                return this->emu->vcpu(0).x64_cpu();
            }

            std::unique_ptr<windows_emulator> emu{};
        };
    }

    TEST_F(DebugStatusTest, AStepsDebugStatusReachesItsHandlerAndLeavesDr6Clear)
    {
        this->cpu().reg(x86_register::dr0, breakpoint);
        this->cpu().reg(x86_register::dr7, dr7_l0);
        this->cpu().reg(x86_register::dr6, stepped_dr6);
        const auto held_dr6 = this->cpu().reg(x86_register::dr6);

        dispatch_single_step(*this->emu, this->emu->vcpu(0));

        const auto context = this->emu->memory.read_memory<CONTEXT64>(this->cpu().reg(x86_register::rsp));
        EXPECT_EQ(held_dr6, context.Dr6);
        EXPECT_EQ(0u, this->cpu().reg(x86_register::dr6) & dr6_status_bits);
    }
}
