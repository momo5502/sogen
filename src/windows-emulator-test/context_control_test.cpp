#include "std_include.hpp"

#include <atomic>
#include <thread>

#include <cpu_context.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtContinueEx(const syscall_context& c, uint64_t thread_context, uint64_t continue_argument);
    NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                     emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> exception_record,
                                     emulator_object<CONTEXT64> thread_context, BOOLEAN first_chance);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t junk_eflags = 0xFFFFFEFF; // every flag but TF
        constexpr uint64_t non_canonical_rip = 0x0000800000001234;
        constexpr uint64_t canonical_rip = 0xFFFF800000001234;
        constexpr uint16_t junk_code_selector = 0x2B;
        constexpr uint16_t junk_stack_selector = 0;
        constexpr uint64_t exception_record_offset = 0x600;
        constexpr uint64_t continue_argument_offset = 0x800;
        constexpr DWORD software_exception_code = 0xE0000001; // a code RaiseException's callers pick

        CONTEXT64 junk_control(const uint16_t code_selector)
        {
            CONTEXT64 value{};
            value.ContextFlags = CONTEXT_CONTROL_64;
            value.EFlags = junk_eflags;
            value.SegCs = code_selector;
            value.SegSs = junk_stack_selector;
            value.Rip = non_canonical_rip;
            return value;
        }

        // The sample's process, whose main thread is loaded on vCPU 0 and whose GDT holds the user selectors.
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

            emulator_object<CONTEXT64> write_context(const CONTEXT64& value) const
            {
                const emulator_object<CONTEXT64> buffer{this->emu.memory, this->storage};
                buffer.write(value);
                return buffer;
            }

            void expect_native_control() const
            {
                EXPECT_EQ(cpu_context::user_code_selector, this->cpu().reg<uint16_t>(x86_register::cs));
                EXPECT_EQ(cpu_context::user_data_selector, this->cpu().reg<uint16_t>(x86_register::ss));
                EXPECT_EQ(canonical_rip, this->cpu().reg(x86_register::rip));
                EXPECT_EQ((junk_eflags & cpu_context::eflags_user_legal) | cpu_context::eflags_interrupt | cpu_context::eflags_always_one,
                          this->cpu().reg<uint32_t>(x86_register::eflags));
            }
        };
    }

    TEST(ContextControlTest, SetContextKeepsWhatWindowsKeepsOfTheControlRegisters)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        const auto buffer = process.write_context(junk_control(junk_code_selector));
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetContextThread(process.context(), CURRENT_THREAD, buffer));
        process.expect_native_control();
    }

    TEST(ContextControlTest, ContinueKeepsWhatWindowsKeepsOfTheControlRegisters)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        const auto buffer = process.write_context(junk_control(junk_code_selector));
        const auto continue_argument = process.storage + continue_argument_offset;
        emu.memory.write_memory(continue_argument, KCONTINUE_ARGUMENT{});
        syscalls::handle_NtContinueEx(process.context(), buffer.value(), continue_argument);
        process.expect_native_control();
    }

    TEST(ContextControlTest, ContextsWithoutInterruptsEnabledRunWithThem)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        auto value = junk_control(cpu_context::user_code_selector);
        value.EFlags = 0;
        value.Rip = canonical_rip;
        const auto buffer = process.write_context(value);
        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtSetContextThread(process.context(), CURRENT_THREAD, buffer));
        EXPECT_EQ(cpu_context::eflags_interrupt | cpu_context::eflags_always_one, process.cpu().reg<uint32_t>(x86_register::eflags));
    }

    TEST(ContextControlTest, AWow64ProcessKeepsThe64BitSelectorAndTakesAnyOtherAsThe32BitOne)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        cpu_context::restore(process.cpu(), junk_control(cpu_context::user_code_selector), true);
        EXPECT_EQ(cpu_context::user_code_selector, process.cpu().reg<uint16_t>(x86_register::cs));
        EXPECT_EQ(canonical_rip, process.cpu().reg(x86_register::rip));

        cpu_context::restore(process.cpu(), junk_control(junk_code_selector), true);
        EXPECT_EQ(cpu_context::wow64_code_selector, process.cpu().reg<uint16_t>(x86_register::cs));
        EXPECT_EQ(uint64_t{static_cast<uint32_t>(non_canonical_rip)}, process.cpu().reg(x86_register::rip));
        EXPECT_EQ(cpu_context::user_data_selector, process.cpu().reg<uint16_t>(x86_register::ss));
    }

    // NtRaiseException's context goes to KxContextToKframes unverified (KiRaiseException runs KeVerifyContextRecord only
    // for a thread with a CET user shadow stack), so a native process that raises with another selector than the 64-bit
    // one resumes with the 32-bit one and a 32-bit RIP when a debugger continues the exception.
    TEST(ContextControlTest, AnUnverifiedContextOfANativeProcessTakesAnyOtherSelectorAsThe32BitOne)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        cpu_context::restore(process.cpu(), junk_control(junk_code_selector), false, cpu_context::context_verification::unverified);
        EXPECT_EQ(cpu_context::wow64_code_selector, process.cpu().reg<uint16_t>(x86_register::cs));
        EXPECT_EQ(uint64_t{static_cast<uint32_t>(non_canonical_rip)}, process.cpu().reg(x86_register::rip));
        EXPECT_EQ(cpu_context::user_data_selector, process.cpu().reg<uint16_t>(x86_register::ss));
    }

    // KiRaiseException puts the raising context in the trap frame before KiDispatchException, which builds the handlers'
    // CONTEXT from it: with no debugger attached, the handlers see the control registers as KxContextToKframes keeps them,
    // EFlags with bit 1 clear as the trap frame holds it.
    TEST(ContextControlTest, ARaisedExceptionsHandlersSeeTheControlRegistersWindowsKeeps)
    {
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        auto value = junk_control(junk_code_selector);
        value.Rsp = process.cpu().reg(x86_register::rsp);
        const auto buffer = process.write_context(value);
        const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> record{emu.memory, process.storage + exception_record_offset};
        EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> raised{};
        raised.ExceptionCode = software_exception_code;
        record.write(raised);

        EXPECT_EQ(STATUS_SUCCESS, syscalls::handle_NtRaiseException(process.context(), record, buffer, TRUE));

        const auto dispatched = emu.memory.read_memory<CONTEXT64>(process.cpu().reg(x86_register::rsp));
        EXPECT_EQ(cpu_context::wow64_code_selector, dispatched.SegCs);
        EXPECT_EQ(cpu_context::user_data_selector, dispatched.SegSs);
        EXPECT_EQ(uint64_t{static_cast<uint32_t>(non_canonical_rip)}, dispatched.Rip);
        EXPECT_EQ((junk_eflags & cpu_context::eflags_user_legal) | cpu_context::eflags_interrupt, dispatched.EFlags);
    }

    // When a debugger declines the first chance, KiDispatchException enters the 64-bit dispatcher with the 64-bit code
    // selector and the trap flag clear in the trap frame, and the handlers' CONTEXT keeps the raised ones: a native
    // process that raised with the 32-bit selector, which the debugger was shown, reaches its handlers, not the WOW64
    // gate.
    TEST(ContextControlTest, AnExceptionADebuggerDeclinesEntersThe64BitDispatcher)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        auto emu = create_sample_emulator();
        const sample_process process{emu};
        auto value = junk_control(cpu_context::wow64_code_selector);
        value.EFlags |= cpu_context::eflags_trap;
        value.Rsp = process.cpu().reg(x86_register::rsp);
        const auto buffer = process.write_context(value);
        const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>> record{emu.memory, process.storage + exception_record_offset};
        EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>> raised{};
        raised.ExceptionCode = software_exception_code;
        record.write(raised);

        emu.attach_debugger();
        std::atomic_bool returned{};
        std::optional<process_debug_event> reported{};
        std::thread debugger([&] {
            while (!returned)
            {
                if ((reported = emu.debugger().take_event()))
                {
                    emu.debugger().continue_event(reported->thread_id, static_cast<uint32_t>(DBG_EXCEPTION_NOT_HANDLED));
                    return;
                }
                std::this_thread::yield();
            }
        });

        NTSTATUS status{};
        emu.synchronize([&] { status = syscalls::handle_NtRaiseException(process.context(), record, buffer, TRUE); });
        returned = true;
        debugger.join();

        ASSERT_TRUE(reported.has_value());
        EXPECT_EQ(software_exception_code, reported->exception_code);
        EXPECT_EQ(STATUS_SUCCESS, status);
        EXPECT_EQ(emu.process.ki_user_exception_dispatcher, process.cpu().reg(x86_register::rip));
        EXPECT_EQ(cpu_context::user_code_selector, process.cpu().reg<uint16_t>(x86_register::cs));
        EXPECT_EQ(0u, process.cpu().reg<uint32_t>(x86_register::eflags) & cpu_context::eflags_trap);

        const auto dispatched = emu.memory.read_memory<CONTEXT64>(process.cpu().reg(x86_register::rsp));
        EXPECT_EQ(cpu_context::wow64_code_selector, dispatched.SegCs);
        EXPECT_EQ(uint64_t{static_cast<uint32_t>(non_canonical_rip)}, dispatched.Rip);
        EXPECT_EQ(cpu_context::eflags_trap, dispatched.EFlags & cpu_context::eflags_trap);
    }
}
