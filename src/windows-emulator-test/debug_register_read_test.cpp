#include "emulation_test_utils.hpp"
#include <exception_dispatch.hpp>
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::array<uint64_t, 4> breakpoints{0x10000, 0x20000, 0x30000, 0x40000};
        // DR6 after a single step: BS and the reserved bits the processor reads as 1.
        constexpr uint64_t stepped_dr6 = 0xFFFF4FF0;
        constexpr uint64_t dr7_l0 = 0x1;
        constexpr uint64_t dr7_le = 0x100;
        constexpr uint64_t dr7_rw0_write = 0x10000;
        constexpr uint64_t dr7_reads_as_one = 0x400;
        constexpr uint64_t buffer_junk = 0xDEADBEEF;

        class DebugRegisterReadTest : public testing::Test
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
                this->storage = this->emu->memory.allocate_memory(0x1000, memory_permission::read_write);
                const auto thread =
                    this->emu->process.threads.store(emulator_thread{this->emu->memory, this->emu->process, 0, 0, 0x100000, 0, 8, true});
                auto* active = this->emu->process.threads.get(thread);
                this->emu->vcpu(0).active_thread = active;
                this->cpu().reg(x86_register::rsp, active->stack_base + active->stack_size - 0x1000);
                // No GDT before process setup: a dispatch then reads no descriptor for CS and stays 64-bit.
                this->cpu().load_gdt(0, 0);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            x86_64_cpu& cpu()
            {
                return this->emu->vcpu(0).x64_cpu();
            }

            // Keeps what the processor holds afterwards: DR7 reads its reserved bit 10 as 1.
            void set_debug_registers(const uint64_t dr7)
            {
                this->cpu().reg(x86_register::dr0, breakpoints[0]);
                this->cpu().reg(x86_register::dr1, breakpoints[1]);
                this->cpu().reg(x86_register::dr2, breakpoints[2]);
                this->cpu().reg(x86_register::dr3, breakpoints[3]);
                this->cpu().reg(x86_register::dr6, stepped_dr6);
                this->cpu().reg(x86_register::dr7, dr7);
                this->held_dr6 = this->cpu().reg(x86_register::dr6);
                this->held_dr7 = this->cpu().reg(x86_register::dr7);
            }

            // NtGetContextThread of the current thread into a buffer that held junk.
            CONTEXT64 thread_context()
            {
                const emulator_object<CONTEXT64> buffer{this->emu->memory, this->storage};
                CONTEXT64 value{};
                value.ContextFlags = CONTEXT_DEBUG_REGISTERS_64;
                value.Dr0 = value.Dr1 = value.Dr2 = value.Dr3 = value.Dr6 = value.Dr7 = buffer_junk;
                value.DebugControl = value.LastBranchToRip = value.LastBranchFromRip = buffer_junk;
                value.LastExceptionToRip = value.LastExceptionFromRip = buffer_junk;
                buffer.write(value);
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtGetContextThread(this->context(), CURRENT_THREAD, buffer));
                return buffer.read();
            }

            // The CONTEXT KiUserExceptionDispatcher gets for a single step.
            CONTEXT64 step_context()
            {
                dispatch_single_step(*this->emu, this->emu->vcpu(0));
                return this->emu->memory.read_memory<CONTEXT64>(this->cpu().reg(x86_register::rsp));
            }

            static void expect_no_debug_registers(const CONTEXT64& context)
            {
                EXPECT_EQ(0u, context.Dr0);
                EXPECT_EQ(0u, context.Dr1);
                EXPECT_EQ(0u, context.Dr2);
                EXPECT_EQ(0u, context.Dr3);
                EXPECT_EQ(0u, context.Dr6);
                EXPECT_EQ(0u, context.Dr7);
            }

            void expect_debug_registers(const CONTEXT64& context) const
            {
                EXPECT_EQ(breakpoints[0], context.Dr0);
                EXPECT_EQ(breakpoints[1], context.Dr1);
                EXPECT_EQ(breakpoints[2], context.Dr2);
                EXPECT_EQ(breakpoints[3], context.Dr3);
                EXPECT_EQ(this->held_dr6, context.Dr6);
                EXPECT_EQ(this->held_dr7, context.Dr7);
            }

            std::unique_ptr<windows_emulator> emu{};
            uint64_t storage{};
            uint64_t held_dr6{};
            uint64_t held_dr7{};
        };
    }

    TEST_F(DebugRegisterReadTest, ContextsShowNoDebugRegistersWhileDr7EnablesNone)
    {
        for (const auto dr7 : {uint64_t{0}, dr7_rw0_write, dr7_reads_as_one})
        {
            this->set_debug_registers(dr7);
            expect_no_debug_registers(this->thread_context());
        }
    }

    TEST_F(DebugRegisterReadTest, ContextsShowTheDebugRegistersWhileDr7EnablesOne)
    {
        for (const auto dr7 : {dr7_l0, dr7_le, dr7_l0 | dr7_rw0_write | dr7_reads_as_one})
        {
            this->set_debug_registers(dr7);
            this->expect_debug_registers(this->thread_context());
        }
    }

    TEST_F(DebugRegisterReadTest, ContextsClearTheLastBranchRecordsAndKeepDebugControl)
    {
        this->set_debug_registers(dr7_l0);
        const auto context = this->thread_context();
        EXPECT_EQ(buffer_junk, context.DebugControl);
        EXPECT_EQ(0u, context.LastBranchToRip);
        EXPECT_EQ(0u, context.LastBranchFromRip);
        EXPECT_EQ(0u, context.LastExceptionToRip);
        EXPECT_EQ(0u, context.LastExceptionFromRip);
    }

    TEST_F(DebugRegisterReadTest, AStepsExceptionContextShowsNoDebugRegistersWhileDr7EnablesNone)
    {
        this->set_debug_registers(0);
        expect_no_debug_registers(this->step_context());
    }

    TEST_F(DebugRegisterReadTest, AStepsExceptionContextShowsTheDebugRegistersWhileDr7EnablesOne)
    {
        this->set_debug_registers(dr7_l0);
        this->expect_debug_registers(this->step_context());
    }
}
