#include "std_include.hpp"

#include <cpu_context.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtContinueEx(const syscall_context& c, uint64_t thread_context, uint64_t continue_argument);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t defaults_with_daz = 0x1FC0;
        constexpr uint32_t junk_mxcsr = 0xFFFFFFFF;
        constexpr uint16_t junk_x87_control = 0xFFFF;
        constexpr uint16_t condition_codes = 0x0700; // C0, C1 and C2 with TOP 0
        constexpr BYTE only_register_0_in_use = 0x01;
        constexpr M128A one{.Low = 0x8000000000000000, .High = 0x3FFF}; // 1.0 as the x87 holds it
        constexpr M128A pattern{.Low = 0x5555555566666666, .High = 0x7777777788888888};
        constexpr uint64_t continue_argument_offset = 0x800;

        // The sample's process, whose main thread is loaded on vCPU 0.
        struct sample_process
        {
            explicit sample_process(windows_emulator& emulator)
                : emu(emulator)
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            windows_emulator& emu;
            uint64_t storage{};

            syscall_context context() const
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            x86_64_cpu& cpu() const
            {
                return this->emu.vcpu(0).x64_cpu();
            }

            CONTEXT64 read() const
            {
                const emulator_object<CONTEXT64> buffer{this->emu.memory, this->storage};
                CONTEXT64 value{};
                value.ContextFlags = CONTEXT_FLOATING_POINT_64;
                buffer.write(value);
                EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtGetContextThread(this->context(), CURRENT_THREAD, buffer));
                return buffer.read();
            }

            void continue_with(const CONTEXT64& value) const
            {
                this->emu.memory.write_memory(this->storage, value);
                this->emu.memory.write_memory(this->storage + continue_argument_offset, KCONTINUE_ARGUMENT{});
                syscalls::handle_NtContinueEx(this->context(), this->storage, this->storage + continue_argument_offset);
            }
        };
    }

    TEST(ContextFloatingPointTest, AReadReportsMxcsrAndTheXmmRegisters)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        process.cpu().reg<uint32_t>(x86_register::mxcsr, defaults_with_daz);
        process.cpu().reg<M128A>(x86_register::xmm6, pattern);

        const auto context = process.read();
        EXPECT_EQ(defaults_with_daz, context.MxCsr);
        EXPECT_EQ(defaults_with_daz, context.FltSave.MxCsr);
        EXPECT_EQ(cpu_context::mxcsr_legal, context.FltSave.MxCsr_Mask);
        EXPECT_EQ(pattern.Low, context.Xmm6.Low);
        EXPECT_EQ(pattern.High, context.Xmm6.High);
    }

    TEST(ContextFloatingPointTest, ContinueKeepsWhatWindowsKeepsOfTheFloatingPointState)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};

        CONTEXT64 value{};
        value.ContextFlags = CONTEXT_FLOATING_POINT_64;
        value.MxCsr = junk_mxcsr;
        value.FltSave.MxCsr = junk_mxcsr;
        value.FltSave.ControlWord = junk_x87_control;
        value.FltSave.StatusWord = condition_codes;
        value.FltSave.TagWord = only_register_0_in_use;
        value.FltSave.FloatRegisters[0] = one;
        value.Xmm6 = pattern;
        process.continue_with(value);

        constexpr auto kept_x87_control =
            static_cast<uint16_t>((junk_x87_control & cpu_context::x87_control_legal) | cpu_context::x87_control_always_one);
        auto& cpu = process.cpu();
        EXPECT_EQ(cpu_context::mxcsr_legal, cpu.reg<uint32_t>(x86_register::mxcsr));
        EXPECT_EQ(kept_x87_control, cpu.reg<uint16_t>(x86_register::fpcw));
        EXPECT_EQ(condition_codes, cpu.reg<uint16_t>(x86_register::fpsw));
        EXPECT_EQ(only_register_0_in_use, cpu.reg<uint16_t>(x86_register::fptag));
        EXPECT_EQ(one.Low, cpu.reg<M128A>(x86_register::st0).Low);
        EXPECT_EQ(pattern.Low, cpu.reg<M128A>(x86_register::xmm6).Low);
        EXPECT_EQ(pattern.High, cpu.reg<M128A>(x86_register::xmm6).High);

        const auto context = process.read();
        EXPECT_EQ(cpu_context::mxcsr_legal, context.MxCsr);
        EXPECT_EQ(cpu_context::mxcsr_legal, context.FltSave.MxCsr);
        EXPECT_EQ(kept_x87_control, context.FltSave.ControlWord);
        EXPECT_EQ(only_register_0_in_use, context.FltSave.TagWord);
    }
}
