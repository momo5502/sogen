#include "std_include.hpp"

#include <cpu_context.hpp>
#include <exception_dispatch.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtContinueEx(const syscall_context& c, uint64_t thread_context, uint64_t continue_argument);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t dr7_last_branch = 0x100;
        constexpr uint64_t dr7_branch_trace = 0x200;
        // nop; nop; jmp +0; nop; nop
        constexpr std::array<uint8_t, 6> nops_around_a_jump{0x90, 0x90, 0xEB, 0x00, 0x90, 0x90};
        constexpr uint64_t first_nop_end = 1;
        constexpr uint64_t jump_target = 4;
        // syscall; jmp +0; nop; nop
        constexpr std::array<uint8_t, 6> system_call_then_jump{0x0F, 0x05, 0xEB, 0x00, 0x90, 0x90};
        constexpr uint64_t jump_after_system_call_target = 4;
        // KiUserExceptionDispatcher finds the EXCEPTION_RECORD after the 0x4F0 bytes of its CONTEXT.
        constexpr uint64_t exception_record_offset = 0x4F0;
        constexpr uint64_t continue_argument_offset = 0x800;
        constexpr uint64_t thread_context_offset = 0x1000;
        constexpr size_t storage_size = 0x2000;
        constexpr size_t code_and_stack_size = 0x2000;
        constexpr auto single_step = static_cast<DWORD>(STATUS_SINGLE_STEP);

        struct trap_flag_run
        {
            uint64_t exception_rip{};
            DWORD status{};
            DWORD parameters{};
        };

        struct stepper_options
        {
            bool instruction_precision{true};
        };

        struct run_options
        {
            uint64_t rax{};
            uint64_t rdx{};
            uint64_t r10{};
            bool trap_flag{true};
        };

        void write_address32(const std::span<uint8_t> bytes, const size_t offset, const uint64_t address)
        {
            const auto value = static_cast<uint32_t>(address);
            memcpy(bytes.data() + offset, &value, sizeof(value));
        }

        class trap_flag_stepper
        {
          public:
            explicit trap_flag_stepper(const stepper_options& options = {})
                : emu_(create_sample_emulator(
                      emulator_settings{.use_relative_time = true, .use_instruction_precision = options.instruction_precision}))
            {
                this->emu_.setup_process_if_necessary();
                this->code_ =
                    this->emu_.memory.allocate_memory(code_and_stack_size, memory_permission::all, false, DEFAULT_ALLOCATION_ADDRESS_32BIT);
                this->storage_ = this->emu_.memory.allocate_memory(storage_size, memory_permission::read_write);
            }

            uint64_t code_address() const
            {
                return this->code_;
            }

            bool uses_instruction_precision() const
            {
                return this->emu_.uses_instruction_precision();
            }

            uint64_t write_thread_context(const CONTEXT64& context)
            {
                this->emu_.memory.write_memory(this->storage_ + thread_context_offset, context);
                return this->storage_ + thread_context_offset;
            }

            uint32_t syscall_id(const std::string_view name)
            {
                const auto* ntdll = this->emu_.mod_manager.ntdll;
                const auto symbol = std::ranges::find_if(ntdll->exports, [&](const exported_symbol& entry) { return entry.name == name; });
                const auto image = this->emu_.memory.read_memory(ntdll->image_base, static_cast<size_t>(ntdll->size_of_image));
                return this->emu_.abi().syscall_abi().scan_syscall_id(*symbol, image).value();
            }

            trap_flag_run run(const std::span<const uint8_t> code, const uint64_t dr7, const run_options& options = {})
            {
                auto& cpu = this->emu_.vcpu(0).x64_cpu();
                const syscall_context c{
                    .win_emu = this->emu_, .emu = this->emu_.vcpu(0).cpu, .vcpu = this->emu_.vcpu(0), .proc = this->emu_.process};
                this->emu_.memory.write_memory(this->code_, code.data(), code.size());

                CONTEXT64 context{};
                context.ContextFlags = CONTEXT_CONTROL_64 | CONTEXT_INTEGER_64 | CONTEXT_DEBUG_REGISTERS_64;
                context.Rip = this->code_;
                context.Rsp = cpu.reg(x86_register::rsp);
                context.Rax = options.rax;
                context.Rdx = options.rdx;
                context.R10 = options.r10;
                context.EFlags = options.trap_flag ? cpu_context::eflags_trap : 0;
                context.Dr7 = dr7;
                this->emu_.memory.write_memory(this->storage_, context);
                this->emu_.memory.write_memory(this->storage_ + continue_argument_offset, KCONTINUE_ARGUMENT{});
                syscalls::handle_NtContinueEx(c, this->storage_, this->storage_ + continue_argument_offset);

                auto* dispatcher_reached = this->emu_.emu().hook_memory_execution(this->emu_.process.ki_user_exception_dispatcher,
                                                                                  [&](cpu_interface&, uint64_t) { this->emu_.stop(); });
                this->emu_.start();
                this->emu_.emu().delete_hook(dispatcher_reached);

                const auto rsp = cpu.reg(x86_register::rsp);
                const auto record =
                    this->emu_.memory.read_memory<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>(rsp + exception_record_offset);
                return {
                    .exception_rip = this->emu_.memory.read_memory<CONTEXT64>(rsp).Rip - this->code_,
                    .status = record.ExceptionCode,
                    .parameters = record.NumberParameters,
                };
            }

          private:
            windows_emulator emu_;
            uint64_t code_{};
            uint64_t storage_{};
        };
    }

    TEST(BranchStepTest, ATrapFlagStepTrapsAfterEachInstruction)
    {
        const auto run = trap_flag_stepper{}.run(nops_around_a_jump, 0);
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(first_nop_end, run.exception_rip);
        EXPECT_EQ(0u, run.parameters);
    }

    TEST(BranchStepTest, Dr7GeMakesATrapFlagStepTrapAfterTheNextTakenBranch)
    {
        const auto run = trap_flag_stepper{}.run(nops_around_a_jump, dr7_branch_trace);
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(jump_target, run.exception_rip);
        EXPECT_EQ(0u, run.parameters);
    }

    TEST(BranchStepTest, Dr7LeAndGeReportTheStepAsABranchTrace)
    {
        const auto run = trap_flag_stepper{}.run(nops_around_a_jump, dr7_last_branch | dr7_branch_trace);
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(jump_target, run.exception_rip);
        EXPECT_EQ(2u, run.parameters);
    }

    TEST(BranchStepTest, Dr7GeClassifiesAStepByTheBytesItsInstructionHadBeforeItRan)
    {
        // mov byte ptr [rip - 7], 0xEB, which turns its own first byte into a short jump; nop; jmp +0; nop; nop
        constexpr std::array<uint8_t, 12> self_rewriting_move{0xC6, 0x05, 0xF9, 0xFF, 0xFF, 0xFF, 0xEB, 0x90, 0xEB, 0x00, 0x90, 0x90};
        constexpr uint64_t real_jump_target = 10;

        const auto run = trap_flag_stepper{}.run(self_rewriting_move, dr7_branch_trace);
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(real_jump_target, run.exception_rip);
    }

    TEST(BranchStepTest, Dr7GeClassifiesAStepThatPopfStartsByTheBytesItsInstructionHadBeforeItRan)
    {
        // push 0x302 (TF, IF and the always-one bit); popfq; then the self-rewriting mov; nop; jmp +0; nop; nop
        constexpr std::array<uint8_t, 18> popf_then_self_rewriting_move{0x68, 0x02, 0x03, 0x00, 0x00, 0x9D, 0xC6, 0x05, 0xF9,
                                                                        0xFF, 0xFF, 0xFF, 0xEB, 0x90, 0xEB, 0x00, 0x90, 0x90};
        constexpr uint64_t real_jump_target = 16;

        trap_flag_stepper stepper{};
        if (!stepper.uses_instruction_precision())
        {
            GTEST_SKIP() << "Only instruction precision sees the instruction a step that POPF starts runs";
        }

        const auto run = stepper.run(popf_then_self_rewriting_move, dr7_branch_trace, {.trap_flag = false});
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(real_jump_target, run.exception_rip);
    }

    TEST(BranchStepTest, Dr7GeFindsATakenJumpAfterASystemCallWithoutInstructionPrecision)
    {
        trap_flag_stepper stepper{{.instruction_precision = false}};
        const auto run = stepper.run(system_call_then_jump, dr7_branch_trace, {.rax = stepper.syscall_id("NtYieldExecution")});
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(jump_after_system_call_target, run.exception_rip);
        EXPECT_EQ(0u, run.parameters);
    }

    TEST(BranchStepTest, Dr7GeFindsATakenJumpAfterASystemCallThatSetsTheThreadsOwnDebugRegisters)
    {
        trap_flag_stepper stepper{{.instruction_precision = false}};
        CONTEXT64 debug_registers{};
        debug_registers.ContextFlags = CONTEXT_DEBUG_REGISTERS_64;
        debug_registers.Dr7 = dr7_branch_trace;

        const auto run = stepper.run(system_call_then_jump, dr7_branch_trace,
                                     {.rax = stepper.syscall_id("NtSetContextThread"),
                                      .rdx = stepper.write_thread_context(debug_registers),
                                      .r10 = CURRENT_THREAD.bits});
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(jump_after_system_call_target, run.exception_rip);
        EXPECT_EQ(0u, run.parameters);
    }

    TEST(BranchStepTest, Dr7GeReportsAFarJumpFrom32BitTo64BitCode)
    {
        // mov esp, <stack top>; push 0x2B; push <stack top>; push 0x302 (TF, IF and the always-one bit); push 0x23;
        // push <32-bit code>; iretq; then 32-bit code: jmp far 0x33:<64-bit code>; then 64-bit code: nop; jmp +0; nop; nop
        std::array<uint8_t, 38> code{0xBC, 0, 0, 0, 0, 0x6A, 0x2B, 0x68, 0, 0, 0, 0, 0x68, 0x02, 0x03, 0x00, 0x00, 0x6A, 0x23,
                                     0x68, 0, 0, 0, 0, 0x48, 0xCF, 0xEA, 0, 0, 0, 0, 0x33, 0x00, 0x90, 0xEB, 0x00, 0x90, 0x90};
        constexpr size_t stack_top_field = 1;
        constexpr size_t stack_image_field = 8;
        constexpr size_t code32_field = 20;
        constexpr size_t code64_field = 27;
        constexpr uint64_t code64_offset = 33;
        constexpr uint64_t code32_offset = 26;

        trap_flag_stepper stepper{};
        const auto base = stepper.code_address();
        // push imm32 sign-extends its operand.
        ASSERT_LE(base + code_and_stack_size, static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));
        write_address32(code, stack_top_field, base + code_and_stack_size);
        write_address32(code, stack_image_field, base + code_and_stack_size);
        write_address32(code, code32_field, base + code32_offset);
        write_address32(code, code64_field, base + code64_offset);

        const auto run = stepper.run(code, dr7_branch_trace, {.trap_flag = false});
        EXPECT_EQ(single_step, run.status);
        EXPECT_EQ(code64_offset, run.exception_rip);
    }

    TEST(BranchStepTest, FarJumpsAndCallsAreTakenBranchesIn32BitCode)
    {
        // jmp far 0x23:0x1000 and call far 0x23:0x1000
        constexpr std::array<uint8_t, 7> far_jump{0xEA, 0x00, 0x10, 0x00, 0x00, 0x23, 0x00};
        constexpr std::array<uint8_t, 7> far_call{0x9A, 0x00, 0x10, 0x00, 0x00, 0x23, 0x00};

        EXPECT_TRUE(is_taken_branch(far_jump, 0, 0, true));
        EXPECT_TRUE(is_taken_branch(far_call, 0, 0, true));
    }
}
