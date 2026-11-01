#include <backend_selection.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>
#include <platform/compiler.hpp>
#include <utils/finally.hpp>

#ifdef OS_MAC
#include <sys/mman.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t code_address = 0x10000;
        constexpr uint64_t data_address = 0x20000;
        constexpr uint64_t stack_address = 0x30000;

        constexpr uint32_t svc(const uint16_t id)
        {
            return 0xD4000001 | (static_cast<uint32_t>(id) << 5);
        }

        class Arm64BackendTest : public testing::Test
        {
          protected:
            std::unique_ptr<arm64_emulator> machine = create_arm64_emulator();
            memory_manager memory{*machine};

            void SetUp() override
            {
                ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::all));
                ASSERT_TRUE(memory.allocate_memory(data_address, 0x1000, memory_permission::read_write));
                ASSERT_TRUE(memory.allocate_memory(stack_address, 0x1000, memory_permission::read_write));
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::sp, stack_address + 0xFF0);
            }

            template <size_t N>
            void load_code(const std::array<uint32_t, N>& code)
            {
                machine->write_memory(code_address, code);
            }
        };
    }

    TEST_F(Arm64BackendTest, SyscallReturnsToEl0WithArgumentsAndRegistersIntact)
    {
        load_code(std::array{0x92800000u, 0xD2800541u, svc(0x2C), 0x8B010000u, 0xF9000100u, svc(0xFFFF), 0xD4200000u});
        const auto register_id = [](const int index) {
            switch (index)
            {
            case 29:
                return arm64_register::x29;
            case 30:
                return arm64_register::x30;
            default:
                return static_cast<arm64_register>(static_cast<int>(arm64_register::x0) + index);
            }
        };
        for (int i = 2; i < 31; ++i)
        {
            machine->reg(register_id(i), 0xABC000ull + i);
        }
        machine->reg(arm64_register::x8, data_address);
        machine->reg(arm64_register::nzcv, 0xA0000000u);
        const std::array<uint64_t, 2> vector{0x123456789ABCDEF0ull, 0xFEDCBA9876543210ull};
        ASSERT_EQ(machine->write_register(arm64_register::q17, vector.data(), sizeof(vector)), sizeof(vector));

        int calls{};
        auto* hook = machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw_cpu, const uint64_t id) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw_cpu);
            EXPECT_EQ(&cpu, &machine->get_cpu(0));
            EXPECT_EQ(cpu.index(), 0u);
            EXPECT_EQ(cpu.reg(arm64_register::pstate) & 0x1F, 0u);
            EXPECT_EQ(cpu.reg(arm64_register::nzcv), 0xA0000000u);
            EXPECT_EQ(cpu.read_stack_pointer(), stack_address + 0xFF0);
            for (int i = 2; i < 31; ++i)
            {
                EXPECT_EQ(cpu.reg(register_id(i)), i == 8 ? data_address : 0xABC000ull + i) << "x" << i;
            }
            std::array<uint64_t, 2> observed{};
            cpu.read_register(arm64_register::q17, observed.data(), sizeof(observed));
            EXPECT_EQ(observed, vector);

            ++calls;
            if (calls == 1)
            {
                EXPECT_EQ(id, 0x2Cu);
                EXPECT_EQ(cpu.read_instruction_pointer(), code_address + 8);
                EXPECT_EQ(cpu.reg(arm64_register::x0), ~0ull);
                EXPECT_EQ(cpu.reg(arm64_register::x1), 42u);
                cpu.reg(arm64_register::x0, 100);
            }
            else
            {
                EXPECT_EQ(id, 0xFFFFu);
                EXPECT_EQ(cpu.read_instruction_pointer(), code_address + 20);
                EXPECT_EQ(cpu.reg(arm64_register::x0), 142u);
                EXPECT_EQ(cpu.read_memory<uint64_t>(data_address), 142u);
                cpu.stop();
            }
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 24);
        machine->delete_hook(hook);
    }

    TEST_F(Arm64BackendTest, FinalizedPcCanRetriggerAndStoppedCpuCanRestart)
    {
        load_code(std::array{svc(7), svc(9), 0xD4200000u});
        int calls{};
        auto* hook = machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t id) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            ++calls;
            EXPECT_EQ(id, calls <= 2 ? 7u : 9u);
            if (calls == 1)
            {
                return instruction_hook_continuation::finalized_instruction_pointer;
            }
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
        machine->start();
        EXPECT_EQ(calls, 3);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        machine->delete_hook(hook);
    }

    TEST_F(Arm64BackendTest, NonSvcAndMissingHooksFailExplicitly)
    {
        load_code(std::array{svc(1)});
        try
        {
            machine->start();
            FAIL() << "An unhandled SVC must fail";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_NE(std::string(error.what()).find("without a syscall hook"), std::string::npos);
        }
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        load_code(std::array{0xD4200000u});
        try
        {
            machine->start();
            FAIL() << "BRK must not be treated as an SVC";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_NE(std::string(error.what()).find("guest exception"), std::string::npos) << error.what();
        }
    }

    TEST_F(Arm64BackendTest, UndefinedAndBreakpointInstructionsReachInterruptHooksAtTheFaultingPc)
    {
        // udf #0; brk #0xf000; svc #1
        load_code(std::array{0x00000000u, 0xD43E0000u, svc(1)});
        std::vector<std::pair<int, uint64_t>> exceptions{};
        machine->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            const auto pc = cpu.read_instruction_pointer();
            exceptions.emplace_back(exception_class, pc);
            cpu.reg(arm64_register::pc, pc + 4);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        const std::vector<std::pair<int, uint64_t>> expected{
            {static_cast<int>(arm64_exception_class::undefined_instruction), code_address},
            {static_cast<int>(arm64_exception_class::breakpoint_instruction), code_address + 4},
        };
        EXPECT_EQ(exceptions, expected);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
    }

    TEST_F(Arm64BackendTest, CounterReadsFromEl0ReachTheCounterHook)
    {
        // mrs x3, cntvct_el0; mrs x5, pmccntr_el0; svc #1
        load_code(std::array{0xD53BE043u, 0xD53B9D05u, svc(1)});
        std::vector<uint64_t> destinations{};
        machine->hook_instruction(arm64_hookable_instructions::read_counter, [&](cpu_interface& raw, const uint64_t destination) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            destinations.push_back(destination);
            cpu.reg(static_cast<arm64_register>(destination), 0x1000 + destinations.size());
            return instruction_hook_continuation::skip_instruction;
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        const std::vector<uint64_t> expected{static_cast<uint64_t>(arm64_register::x3), static_cast<uint64_t>(arm64_register::x5)};
        EXPECT_EQ(destinations, expected);
        EXPECT_EQ(machine->reg(arm64_register::x3), 0x1001u);
        EXPECT_EQ(machine->reg(arm64_register::x5), 0x1002u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
    }

    TEST_F(Arm64BackendTest, SelfSynchronizedCounterReadsReachTheCounterHook)
    {
        if (machine->get_name() != "hvf-arm64")
        {
            GTEST_SKIP() << "CNTVCTSS_EL0 needs FEAT_ECV";
        }
        // mrs x7, cntvctss_el0; svc #1
        load_code(std::array{0xD53BE0C7u, svc(1)});
        machine->hook_instruction(arm64_hookable_instructions::read_counter, [&](cpu_interface& raw, const uint64_t destination) {
            EXPECT_EQ(destination, static_cast<uint64_t>(arm64_register::x7));
            dynamic_cast<arm64_cpu&>(raw).reg(static_cast<arm64_register>(destination), 0x4242u);
            return instruction_hook_continuation::skip_instruction;
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x7), 0x4242u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
    }

    TEST_F(Arm64BackendTest, UserSoftwareStepRunsOneInstructionAndReportsTheNextPc)
    {
        // mov x0, #1; add x0, x0, #1; add x0, x0, #1; svc #1
        load_code(std::array{0xD2800020u, 0x91000400u, 0x91000400u, svc(1)});
        constexpr uint64_t software_step = 1u << 21;
        machine->reg(arm64_register::pstate, software_step);
        std::vector<std::pair<int, uint64_t>> exceptions{};
        machine->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            exceptions.emplace_back(exception_class, cpu.read_instruction_pointer());
            EXPECT_EQ(cpu.reg(arm64_register::x0), 1u);
            EXPECT_EQ(cpu.reg(arm64_register::pstate) & software_step, 0u);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        const std::vector<std::pair<int, uint64_t>> expected{{static_cast<int>(arm64_exception_class::software_step), code_address + 4}};
        EXPECT_EQ(exceptions, expected);
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
    }

    TEST_F(Arm64BackendTest, UserSoftwareStepSurvivesExecutionHooksAndCounterReads)
    {
        constexpr uint64_t software_step = 1u << 21;
        std::vector<std::pair<int, uint64_t>> exceptions{};
        machine->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            exceptions.emplace_back(exception_class, dynamic_cast<arm64_cpu&>(raw).read_instruction_pointer());
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->hook_instruction(arm64_hookable_instructions::read_counter, [&](cpu_interface& raw, const uint64_t destination) {
            dynamic_cast<arm64_cpu&>(raw).reg(static_cast<arm64_register>(destination), 7);
            return instruction_hook_continuation::skip_instruction;
        });
        int executions{};
        auto* execution = machine->hook_memory_execution(code_address, [&](cpu_interface&, uint64_t) { ++executions; });
        const std::vector<std::pair<int, uint64_t>> expected{{static_cast<int>(arm64_exception_class::software_step), code_address + 4}};

        // mov x0, #1; svc #1
        load_code(std::array{0xD2800020u, svc(1)});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::pstate, software_step);
        machine->start();
        EXPECT_EQ(executions, 1);
        EXPECT_EQ(exceptions, expected);
        machine->delete_hook(execution);

        // mrs x3, cntvct_el0; svc #1
        exceptions.clear();
        load_code(std::array{0xD53BE043u, svc(1)});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::pstate, software_step);
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x3), 7u);
        EXPECT_EQ(exceptions, expected);
    }

    TEST_F(Arm64BackendTest, UserSoftwareStepStoppedBeforeItsInstructionStaysPending)
    {
        // mov x0, #1; add x0, x0, #1; add x0, x0, #1; svc #1
        load_code(std::array{0xD2800020u, 0x91000400u, 0x91000400u, svc(1)});
        constexpr uint64_t software_step = 1u << 21;
        machine->reg(arm64_register::pstate, software_step);
        std::vector<std::pair<int, uint64_t>> exceptions{};
        machine->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            exceptions.emplace_back(exception_class, dynamic_cast<arm64_cpu&>(raw).read_instruction_pointer());
        });
        bool stopped{};
        machine->hook_memory_range_execution(code_address, 4, [&](cpu_interface& cpu, uint64_t) {
            if (!std::exchange(stopped, true))
            {
                cpu.stop();
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        machine->start();
        EXPECT_TRUE(exceptions.empty());
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        EXPECT_NE(machine->reg(arm64_register::pstate) & software_step, 0u);

        machine->start();
        const std::vector<std::pair<int, uint64_t>> expected{{static_cast<int>(arm64_exception_class::software_step), code_address + 4}};
        EXPECT_EQ(exceptions, expected);
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
    }

    TEST_F(Arm64BackendTest, SoftwareStepRequestedByTheStepHandlerStepsTheNextInstruction)
    {
        // mov x0, #1; add x0, x0, #1; add x0, x0, #1; svc #1
        load_code(std::array{0xD2800020u, 0x91000400u, 0x91000400u, svc(1)});
        constexpr uint64_t software_step = 1u << 21;
        machine->reg(arm64_register::pstate, software_step);
        std::vector<std::pair<int, uint64_t>> exceptions{};
        machine->hook_interrupt([&](cpu_interface& raw, const int exception_class) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            exceptions.emplace_back(exception_class, cpu.read_instruction_pointer());
            cpu.reg(arm64_register::pstate, software_step);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        machine->start();
        constexpr auto step = static_cast<int>(arm64_exception_class::software_step);
        const std::vector<std::pair<int, uint64_t>> expected{{step, code_address + 4}, {step, code_address + 8}, {step, code_address + 12}};
        EXPECT_EQ(exceptions, expected);
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
    }

    TEST_F(Arm64BackendTest, MemoryFaultsReachTheViolationHookAtTheFaultingPc)
    {
        constexpr uint64_t unmapped = 0x50000;
        constexpr uint64_t no_execute = 0x60000;
        ASSERT_TRUE(memory.allocate_memory(no_execute, 0x1000, memory_permission::read_write));
        ASSERT_TRUE(memory.protect_memory(data_address, 0x1000, memory_permission::read));
        // ldr x0, [x8]; str x1, [x9]; blr x10; svc #1
        load_code(std::array{0xF9400100u, 0xF9000121u, 0xD63F0140u, svc(1)});
        machine->reg(arm64_register::x8, unmapped + 8);
        machine->reg(arm64_register::x9, data_address);
        machine->reg(arm64_register::x10, no_execute);

        struct fault
        {
            uint64_t address;
            uint64_t pc;
            memory_operation operation;
            memory_violation_type type;
            bool operator==(const fault&) const = default;
        };

        std::vector<fault> faults{};
        machine->hook_memory_violation(
            [&](cpu_interface& raw, const uint64_t address, size_t, const memory_operation operation, const memory_violation_type type) {
                auto& cpu = dynamic_cast<arm64_cpu&>(raw);
                const auto pc = cpu.read_instruction_pointer();
                faults.push_back({.address = address, .pc = pc, .operation = operation, .type = type});
                // Return from the faulting call, or step over the faulting load/store.
                cpu.reg(arm64_register::pc, operation == memory_operation::exec ? cpu.reg(arm64_register::x30) : pc + 4);
                return memory_violation_continuation::resume;
            });
        bool done{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            done = true;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (int runs = 0; !done && runs < 8; ++runs)
        {
            machine->start();
        }
        ASSERT_TRUE(done);
        const std::vector<fault> expected{
            {.address = unmapped + 8, .pc = code_address, .operation = memory_operation::read, .type = memory_violation_type::unmapped},
            {.address = data_address,
             .pc = code_address + 4,
             .operation = memory_operation::write,
             .type = memory_violation_type::protection},
            {.address = no_execute, .pc = no_execute, .operation = memory_operation::exec, .type = memory_violation_type::protection},
        };
        EXPECT_EQ(faults, expected);
    }

    TEST_F(Arm64BackendTest, ExecutionRangeHooksSeeEachInstructionOnceAndCanBeRemoved)
    {
        // mov x0, #1; add x0, x0, #1; add x0, x0, #1; svc #1
        load_code(std::array{0xD2800020u, 0x91000400u, 0x91000400u, svc(1)});
        std::vector<uint64_t> executed{};
        emulator_hook* hook{};
        hook = machine->hook_memory_range_execution(code_address + 4, 8, [&](cpu_interface&, const uint64_t address) {
            executed.push_back(address);
            if (executed.size() == 2)
            {
                machine->delete_hook(hook);
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(executed, (std::vector<uint64_t>{code_address + 4, code_address + 8}));
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);

        executed.clear();
        machine->reg(arm64_register::pc, code_address);
        machine->start();
        EXPECT_TRUE(executed.empty());
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
    }

    TEST_F(Arm64BackendTest, SingleStepOverAnExecutionHookRunsExactlyOneInstruction)
    {
        // mov x0, #1; add x0, x0, #1; svc #1
        load_code(std::array{0xD2800020u, 0x91000400u, svc(1)});
        int calls{};
        machine->hook_memory_range_execution(code_address, 4, [&](cpu_interface&, uint64_t) { ++calls; });
        machine->start(1);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
        EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
    }

    TEST_F(Arm64BackendTest, ExecutionHookStoppedBeforeItsInstructionFiresAgainForAnotherThread)
    {
        // mov x0, #1; svc #1
        load_code(std::array{0xD2800020u, svc(1)});
        std::vector<uint64_t> teb_at_hook{};
        machine->hook_memory_range_execution(code_address, 4, [&](cpu_interface& cpu, uint64_t) {
            teb_at_hook.push_back(machine->reg(arm64_register::x18));
            if (teb_at_hook.size() == 1)
            {
                cpu.stop();
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        machine->reg(arm64_register::x18, 0x1000);
        machine->start();
        ASSERT_EQ(teb_at_hook.size(), 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);

        // A thread switch: another thread (TEB in X18) resumes at the same hooked instruction.
        machine->reg(arm64_register::x18, 0x2000);
        machine->start();
        EXPECT_EQ(teb_at_hook, (std::vector<uint64_t>{0x1000, 0x2000}));
        EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
    }

    TEST_F(Arm64BackendTest, MmioReadsComeFromTheReadCallback)
    {
        constexpr uint64_t mmio = 0x70000;
        uint64_t value = 0x1111;
        memory.allocate_mmio(
            mmio, 0x1000,
            [&](const uint64_t offset, void* data, const size_t size) {
                std::memset(data, 0, size);
                if (offset < sizeof(value))
                {
                    std::memcpy(data, reinterpret_cast<const std::byte*>(&value) + offset,
                                std::min(size, static_cast<size_t>(sizeof(value) - offset)));
                }
            },
            [](uint64_t, const void*, size_t) {});
        // ldr x0, [x8]; svc #1
        load_code(std::array{0xF9400100u, svc(1)});
        machine->reg(arm64_register::x8, mmio);
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x0), 0x1111u);
        value = 0x2222;
        EXPECT_EQ(machine->read_memory<uint64_t>(mmio), 0x2222u);
        machine->reg(arm64_register::pc, code_address);
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x0), 0x2222u);
    }

    TEST_F(Arm64BackendTest, RegisterAliasesAndSavedStateRoundTrip)
    {
        machine->reg(arm64_register::x0, ~0ull);
        machine->reg(arm64_register::w0, 0x12345678u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 0x12345678u);
        machine->reg(arm64_register::wzr, 5);
        EXPECT_EQ(machine->reg(arm64_register::xzr), 0u);
        machine->reg(arm64_register::x18, 0x123000u);
        machine->reg(arm64_register::nzcv, 0x60000000u);
        const std::array<uint64_t, 2> vector{1, 2};
        machine->write_register(arm64_register::v31, vector.data(), sizeof(vector));
        const auto saved = machine->save_registers();
        machine->reg(arm64_register::x18, 0u);
        machine->reg(arm64_register::nzcv, 0u);
        const std::array<uint64_t, 2> empty{};
        machine->write_register(arm64_register::q31, empty.data(), sizeof(empty));
        machine->restore_registers(saved);
        EXPECT_EQ(machine->reg(arm64_register::x18), 0x123000u);
        EXPECT_EQ(machine->reg(arm64_register::nzcv), 0x60000000u);
        std::array<uint64_t, 2> observed{};
        machine->read_register(arm64_register::q31, observed.data(), sizeof(observed));
        EXPECT_EQ(observed, vector);
        EXPECT_THROW(machine->restore_registers({}), std::invalid_argument);
        EXPECT_THROW(machine->reg(arm64_register::pstate, 5u), std::invalid_argument);
        machine->reg<uint32_t>(arm64_register::pstate, 0x40000000u);
        EXPECT_EQ(machine->reg<uint32_t>(arm64_register::pstate), 0x40000000u);
        EXPECT_EQ(machine->reg(arm64_register::nzcv), 0x40000000u);
        EXPECT_THROW(machine->reg(static_cast<arm64_register>(9999)), std::invalid_argument);
        uint64_t too_small{};
        EXPECT_THROW(machine->read_register(arm64_register::q0, &too_small, sizeof(too_small)), std::invalid_argument);
    }

    TEST_F(Arm64BackendTest, CrossPageAccessUnmappingAndRemapping)
    {
        constexpr uint64_t base = 0x40000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x3000, memory_permission::read_write));
        const std::array<uint64_t, 2> pattern{0x12345678u, 0xABCDEFu};
        machine->write_memory(base + 0xFF8, pattern);
        EXPECT_EQ((machine->read_memory<std::array<uint64_t, 2>>(base + 0xFF8)), pattern);
        ASSERT_TRUE(memory.decommit_memory(base + 0x1000, 0x1000));
        std::array<uint64_t, 2> read{};
        EXPECT_FALSE(machine->try_read_memory(base + 0xFF8, read.data(), sizeof(read)));
        EXPECT_FALSE(machine->try_write_memory(base + 0xFF8, read.data(), sizeof(read)));
        EXPECT_EQ(machine->read_memory<uint64_t>(base + 0xFF8), pattern[0]);
        EXPECT_EQ(machine->read_memory<uint64_t>(base + 0x2000), 0u);
        ASSERT_TRUE(memory.commit_memory(base + 0x1000, 0x1000, memory_permission::read_write));
        EXPECT_EQ(machine->read_memory<uint64_t>(base + 0x1000), 0u);
        ASSERT_TRUE(memory.release_memory(base, 0x3000));
        EXPECT_FALSE(machine->try_read_memory(base, read.data(), sizeof(read)));
    }

    TEST_F(Arm64BackendTest, GuestProtectionFaultIsNotMistakenForSvc)
    {
        load_code(std::array{0xF9000100u, svc(1)});
        machine->reg(arm64_register::x8, data_address);
        machine->reg(arm64_register::x0, 42u);
        ASSERT_TRUE(memory.protect_memory(data_address, 0x1000, memory_permission::read));
        int calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface&, uint64_t) {
            ++calls;
            return instruction_hook_continuation::skip_instruction;
        });
        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(machine->read_memory<uint64_t>(data_address), 0u);
    }

    TEST_F(Arm64BackendTest, NormalMemorySupportsUnalignedCrossPageGuestAccess)
    {
        constexpr uint64_t base = 0x40000;
        constexpr uint64_t pattern = 0x123456789ABCDEF0ull;
        ASSERT_TRUE(memory.allocate_memory(base, 0x2000, memory_permission::read_write));
        load_code(std::array{0xF9400100u, 0xF9000120u, svc(1)});
        machine->write_memory(base + 3, pattern);
        machine->reg(arm64_register::x8, base + 3);
        machine->reg(arm64_register::x9, base + 0xFFC);
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            EXPECT_EQ(cpu.reg(arm64_register::x0), pattern);
            EXPECT_EQ(cpu.read_memory<uint64_t>(base + 0xFFC), pattern);
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
    }

    TEST_F(Arm64BackendTest, StageOneTranslationChangesAreVisibleAcrossRuns)
    {
        constexpr uint64_t base = 0x140000000ull;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        load_code(std::array{0xF9400100u, svc(1)});
        machine->reg(arm64_register::x8, base);
        uint64_t expected = 41;
        machine->write_memory(base, expected);
        int calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            EXPECT_EQ(cpu.reg(arm64_register::x0), expected);
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        ASSERT_TRUE(memory.release_memory(base, 0x1000));
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        expected = 57;
        machine->write_memory(base, expected);
        machine->reg(arm64_register::pc, code_address);
        machine->start();
        EXPECT_EQ(calls, 2);
    }

    TEST_F(Arm64BackendTest, SingleStepExecutesOneInstructionIncludingBranchesAndSvc)
    {
        load_code(std::array{0xD28000A0u, 0x14000002u, 0xD2800C60u, 0x91001C00u, svc(3), 0x14000000u});
        int calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t id) {
            EXPECT_EQ(id, 3u);
            EXPECT_EQ(dynamic_cast<arm64_cpu&>(raw).reg(arm64_register::x0), 12u);
            ++calls;
            return instruction_hook_continuation::skip_instruction;
        });
        ASSERT_TRUE(machine->supports_instruction_counting());
        machine->start(1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
        EXPECT_EQ(machine->reg(arm64_register::x0), 5u);
        machine->start(1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        machine->start(1);
        EXPECT_EQ(machine->reg(arm64_register::x0), 12u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
        machine->start(1);
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 20);
        machine->start(3);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 20);
    }

    TEST_F(Arm64BackendTest, FloatingPointSimdAndLargeSystemExtensionAtomicsRunAtEl0)
    {
        // fmov d0, #1.0; fadd d0, d0, d0; ldaddal x1, x2, [x8]; svc #1
        load_code(std::array{0x1E6E1000u, 0x1E602800u, 0xF8E10102u, svc(1)});
        machine->write_memory(data_address, uint64_t{40});
        machine->reg(arm64_register::x1, 2u);
        machine->reg(arm64_register::x8, data_address);
        int calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<arm64_cpu&>(raw);
            EXPECT_EQ(cpu.reg(arm64_register::pstate) & 0x1F, 0u);
            std::array<uint64_t, 2> vector{};
            cpu.read_register(arm64_register::q0, vector.data(), sizeof(vector));
            EXPECT_EQ(vector[0], std::bit_cast<uint64_t>(2.0));
            EXPECT_EQ(cpu.reg(arm64_register::x2), 40u);
            EXPECT_EQ(cpu.read_memory<uint64_t>(data_address), 42u);
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(calls, 1);
    }

    TEST_F(Arm64BackendTest, DebugRegistersRoundTripThroughSavedState)
    {
        machine->reg(arm64_register::bvr0, code_address);
        machine->reg(arm64_register::wvr0, data_address);
        const auto saved = machine->save_registers();
        machine->reg(arm64_register::bvr0, 0u);
        machine->reg(arm64_register::wvr0, 0u);
        machine->restore_registers(saved);
        EXPECT_EQ(machine->reg(arm64_register::bvr0), code_address);
        EXPECT_EQ(machine->reg(arm64_register::wvr0), data_address);
    }

#ifdef OS_MAC
    // MAP_JIT, and the rule this checks, exist only on Apple platforms.
    TEST_F(Arm64BackendTest, HostMemoryBacksGuestPagesOnlyWhenItIsNotExecutable)
    {
        if (machine->get_name() != "hvf-arm64")
        {
            GTEST_SKIP() << "Only Hypervisor.framework maps host pages into a VM";
        }
        constexpr size_t size = 0x4000;
        constexpr uint64_t data_guest = 0x80000;
        constexpr uint64_t jit_guest = 0x90000;
        auto* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        auto* jit = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
        ASSERT_NE(data, MAP_FAILED);
        ASSERT_NE(jit, MAP_FAILED);
        const auto release = utils::finally([&] {
            munmap(data, size);
            munmap(jit, size);
        });

        bool jit_mapped = true;
        try
        {
            jit_mapped = memory.allocate_host_memory_at(jit_guest, size, jit, memory_permission::read_write);
        }
        catch (const std::exception&)
        {
            jit_mapped = false;
        }
        EXPECT_FALSE(jit_mapped);

        constexpr uint64_t value = 0x1122334455667788ull;
        std::memcpy(data, &value, sizeof(value));
        ASSERT_TRUE(memory.allocate_host_memory_at(data_guest, size, data, memory_permission::read_write));
        // ldr x0, [x8]; svc #1
        load_code(std::array{0xF9400100u, svc(1)});
        machine->reg(arm64_register::x8, data_guest);
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        machine->start();
        EXPECT_EQ(machine->reg(arm64_register::x0), value);
    }
#endif

    namespace
    {
        constexpr uint64_t peer_code_address = 0x11000;
        constexpr uint64_t shared_address = 0x40000;
        constexpr auto watchdog_timeout = std::chrono::seconds(10);

        class Arm64MultiVcpuBackendTest : public testing::Test
        {
          protected:
            std::unique_ptr<arm64_emulator> machine{};
            std::optional<memory_manager> memory{};

            void SetUp() override
            {
                if (get_arm64_emulator_backend_from_environment() == backend_type::unicorn)
                {
                    GTEST_SKIP() << "The ARM64 backend of this host runs a single vCPU";
                }

                machine = create_arm64_emulator_from_environment(2);
                memory.emplace(*machine);
                ASSERT_TRUE(machine->supports_multiple_vcpus());
                ASSERT_EQ(machine->vcpu_count(), 2u);
                ASSERT_TRUE(memory->allocate_memory(code_address, 0x2000, memory_permission::read_exec));
                ASSERT_TRUE(memory->allocate_memory(data_address, 0x1000, memory_permission::read_write));
                for (size_t i = 0; i < 2; ++i)
                {
                    cpu(i).reg(arm64_register::pc, i == 0 ? code_address : peer_code_address);
                    cpu(i).reg(arm64_register::sp, stack_address + 0x800 * (i + 1));
                }
            }

            void TearDown() override
            {
                memory.reset();
                machine.reset();
            }

            arm64_cpu& cpu(const size_t index) const
            {
                return machine->get_cpu(index);
            }

            template <size_t N>
            void load_code(const size_t index, const std::array<uint32_t, N>& code)
            {
                machine->write_memory(index == 0 ? code_address : peer_code_address, code);
            }

            // Runs both vCPUs on their own host threads and stops any vCPU still running after the timeout.
            bool run_both()
            {
                std::atomic<size_t> finished{};
                std::array<std::exception_ptr, 2> errors{};
                std::vector<std::thread> threads{};
                for (size_t i = 0; i < 2; ++i)
                {
                    threads.emplace_back([&, i] {
                        try
                        {
                            cpu(i).start();
                        }
                        catch (...)
                        {
                            errors[i] = std::current_exception();
                        }
                        ++finished;
                    });
                }

                const auto deadline = std::chrono::steady_clock::now() + watchdog_timeout;
                while (finished < 2 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const auto timed_out = finished < 2;
                for (size_t i = 0; i < 2; ++i)
                {
                    cpu(i).stop();
                }
                for (auto& thread : threads)
                {
                    thread.join();
                }
                for (const auto& error : errors)
                {
                    if (error)
                    {
                        std::rethrow_exception(error);
                    }
                }
                return !timed_out;
            }

            // Runs one vCPU on a host thread while the caller acts as its peer.
            class background_run
            {
              public:
                explicit background_run(arm64_cpu& cpu)
                    : cpu_(cpu),
                      thread_([this] {
                          try
                          {
                              cpu_.start();
                          }
                          catch (...)
                          {
                              error_ = std::current_exception();
                          }
                          finished_ = true;
                      })
                {
                }

                ~background_run()
                {
                    if (thread_.joinable())
                    {
                        cpu_.stop();
                        thread_.join();
                    }
                }

                background_run(const background_run&) = delete;
                background_run& operator=(const background_run&) = delete;
                background_run(background_run&&) = delete;
                background_run& operator=(background_run&&) = delete;

                bool wait()
                {
                    const auto deadline = std::chrono::steady_clock::now() + watchdog_timeout;
                    while (!finished_ && std::chrono::steady_clock::now() < deadline)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const bool finished = finished_;
                    cpu_.stop();
                    thread_.join();
                    if (error_)
                    {
                        std::rethrow_exception(error_);
                    }
                    return finished;
                }

              private:
                arm64_cpu& cpu_;
                std::atomic_bool finished_{};
                std::exception_ptr error_{};
                std::thread thread_;
            };

            void wait_for_guest_progress(const uint64_t counter_address) const
            {
                const auto deadline = std::chrono::steady_clock::now() + watchdog_timeout;
                while (machine->read_memory<uint64_t>(counter_address) < 1000 && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                ASSERT_GE(machine->read_memory<uint64_t>(counter_address), 1000u) << "The guest loop never ran";
            }
        };
    }

    TEST_F(Arm64MultiVcpuBackendTest, VcpusRunInParallelAndHooksReportTheActingVcpu)
    {
        // ldar x0, [x8]; cbz x0, -4; svc #1
        load_code(0, std::array{0xC8DFFD00u, 0xB4FFFFE0u, svc(1)});
        // mov x0, #1; stlr x0, [x8]; svc #2
        load_code(1, std::array{0xD2800020u, 0xC89FFD00u, svc(2)});
        cpu(0).reg(arm64_register::x8, data_address);
        cpu(1).reg(arm64_register::x8, data_address);

        std::mutex mutex{};
        std::vector<std::pair<size_t, uint64_t>> calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, const uint64_t id) {
            auto& acting = dynamic_cast<arm64_cpu&>(raw);
            EXPECT_EQ(&acting, &cpu(acting.index()));
            EXPECT_EQ(acting.reg(arm64_register::x0), 1u);
            {
                const std::scoped_lock lock(mutex);
                calls.emplace_back(acting.index(), id);
            }
            acting.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_TRUE(run_both()) << "A vCPU spinning on a peer's store never saw it";
        std::ranges::sort(calls);
        EXPECT_EQ(calls, (std::vector<std::pair<size_t, uint64_t>>{{0, 1}, {1, 2}}));
        EXPECT_EQ(cpu(0).read_instruction_pointer(), code_address + 12);
        EXPECT_EQ(cpu(1).read_instruction_pointer(), peer_code_address + 12);
    }

    TEST_F(Arm64MultiVcpuBackendTest, EachVcpuHasItsOwnRegistersAccessibleFromAnyThread)
    {
        cpu(0).reg(arm64_register::x5, 0x55u);
        cpu(1).reg(arm64_register::x5, 0x66u);
        EXPECT_EQ(machine->reg(arm64_register::x5), 0x55u);

        std::thread([&] {
            EXPECT_EQ(cpu(0).reg(arm64_register::x5), 0x55u);
            EXPECT_EQ(cpu(1).reg(arm64_register::x5), 0x66u);
            cpu(1).reg(arm64_register::x6, 0x77u);
        }).join();
        EXPECT_EQ(cpu(1).reg(arm64_register::x6), 0x77u);

        const auto saved = cpu(1).save_registers();
        cpu(0).restore_registers(saved);
        EXPECT_EQ(cpu(0).reg(arm64_register::x5), 0x66u);
        EXPECT_EQ(cpu(0).read_instruction_pointer(), peer_code_address);
        EXPECT_EQ(cpu(0).index(), 0u);
        EXPECT_EQ(cpu(1).index(), 1u);
    }

    TEST_F(Arm64MultiVcpuBackendTest, StopEndsEachSpinningVcpu)
    {
        // add x1, x1, #1; str x1, [x9]; b -8
        constexpr std::array loop{0x91000421u, 0xF9000121u, 0x17FFFFFEu};
        load_code(0, loop);
        load_code(1, loop);
        std::array<std::unique_ptr<background_run>, 2> runs{};
        for (size_t i = 0; i < 2; ++i)
        {
            cpu(i).reg(arm64_register::x9, data_address + 8 * i);
            runs[i] = std::make_unique<background_run>(cpu(i));
        }
        wait_for_guest_progress(data_address);
        wait_for_guest_progress(data_address + 8);
        for (size_t i = 0; i < 2; ++i)
        {
            cpu(i).stop();
            EXPECT_TRUE(runs[i]->wait());
            const auto pc = cpu(i).read_instruction_pointer();
            const auto base = i == 0 ? code_address : peer_code_address;
            EXPECT_TRUE(pc >= base && pc < base + 12) << std::hex << pc;
        }
    }

    TEST_F(Arm64MultiVcpuBackendTest, ReleasingMemoryRevokesAccessOfARunningPeer)
    {
        ASSERT_TRUE(memory->allocate_memory(shared_address, 0x1000, memory_permission::read_write));
        // ldr x0, [x8]; add x1, x1, #1; str x1, [x9]; b -12
        load_code(1, std::array{0xF9400100u, 0x91000421u, 0xF9000121u, 0x17FFFFFDu});
        cpu(1).reg(arm64_register::x8, shared_address);
        cpu(1).reg(arm64_register::x9, data_address);

        std::atomic<uint64_t> fault_address{};
        std::atomic<int> fault_type{-1};
        machine->hook_memory_violation(
            [&](cpu_interface& raw, const uint64_t address, size_t, memory_operation, const memory_violation_type type) {
                fault_address = address;
                fault_type = static_cast<int>(type);
                raw.stop();
                return memory_violation_continuation::resume;
            });

        background_run run{cpu(1)};
        wait_for_guest_progress(data_address);
        ASSERT_TRUE(memory->release_memory(shared_address, 0x1000));
        const auto iterations = machine->read_memory<uint64_t>(data_address);

        ASSERT_TRUE(run.wait()) << "The peer kept reading released memory through a stale translation";
        EXPECT_EQ(fault_address.load(), shared_address);
        EXPECT_EQ(fault_type.load(), static_cast<int>(memory_violation_type::unmapped));
        EXPECT_LE(machine->read_memory<uint64_t>(data_address), iterations + 1);
        EXPECT_EQ(cpu(1).read_instruction_pointer(), peer_code_address);
    }

    TEST_F(Arm64MultiVcpuBackendTest, RemovingWritePermissionRevokesItForARunningPeer)
    {
        ASSERT_TRUE(memory->allocate_memory(shared_address, 0x1000, memory_permission::read_write));
        // add x1, x1, #1; str x1, [x8]; str x1, [x9]; b -12
        load_code(1, std::array{0x91000421u, 0xF9000101u, 0xF9000121u, 0x17FFFFFDu});
        cpu(1).reg(arm64_register::x8, shared_address);
        cpu(1).reg(arm64_register::x9, data_address);

        std::atomic<int> fault_operation{-1};
        std::atomic<int> fault_type{-1};
        machine->hook_memory_violation(
            [&](cpu_interface& raw, uint64_t, size_t, const memory_operation operation, const memory_violation_type type) {
                fault_operation = static_cast<int>(operation);
                fault_type = static_cast<int>(type);
                raw.stop();
                return memory_violation_continuation::resume;
            });

        background_run run{cpu(1)};
        wait_for_guest_progress(data_address);
        ASSERT_TRUE(memory->protect_memory(shared_address, 0x1000, memory_permission::read));
        const auto frozen = machine->read_memory<uint64_t>(shared_address);

        ASSERT_TRUE(run.wait()) << "The peer kept writing through a stale read-write translation";
        EXPECT_EQ(fault_operation.load(), static_cast<int>(memory_operation::write));
        EXPECT_EQ(fault_type.load(), static_cast<int>(memory_violation_type::protection));
        EXPECT_EQ(machine->read_memory<uint64_t>(shared_address), frozen);
    }

    TEST_F(Arm64MultiVcpuBackendTest, PermissionGrantedWhileAPeerRunsIsNotReportedAsAViolation)
    {
        ASSERT_TRUE(memory->allocate_memory(shared_address, 0x1000, memory_permission::read));
        // ldr x0, [x8]; cbz x0, -4; str x0, [x8, #8]; svc #1
        load_code(1, std::array{0xF9400100u, 0xB4FFFFE0u, 0xF9000500u, svc(1)});
        cpu(1).reg(arm64_register::x8, shared_address);

        std::atomic<int> violations{};
        machine->hook_memory_violation([&](cpu_interface& raw, uint64_t, size_t, memory_operation, memory_violation_type) {
            ++violations;
            raw.stop();
            return memory_violation_continuation::resume;
        });
        std::atomic_bool reached{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            reached = true;
            raw.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        background_run run{cpu(1)};
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ASSERT_TRUE(memory->protect_memory(shared_address, 0x1000, memory_permission::read_write));
        machine->write_memory(shared_address, uint64_t{0x1234});

        ASSERT_TRUE(run.wait());
        EXPECT_TRUE(reached);
        EXPECT_EQ(violations.load(), 0);
        EXPECT_EQ(machine->read_memory<uint64_t>(shared_address + 8), 0x1234u);
    }

    TEST_F(Arm64MultiVcpuBackendTest, ExecutionHooksFireOnEveryVcpu)
    {
        // mov x0, #1; svc #1
        load_code(0, std::array{0xD2800020u, svc(1)});
        load_code(1, std::array{0xD2800020u, svc(1)});
        std::mutex mutex{};
        std::vector<size_t> hooked{};
        machine->hook_memory_range_execution(code_address, 0x2000, [&](cpu_interface& raw, const uint64_t address) {
            if (address == code_address || address == peer_code_address)
            {
                const std::scoped_lock lock(mutex);
                hooked.push_back(raw.index());
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            raw.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_TRUE(run_both());
        std::ranges::sort(hooked);
        EXPECT_EQ(hooked, (std::vector<size_t>{0, 1}));
        EXPECT_EQ(cpu(0).reg(arm64_register::x0), 1u);
        EXPECT_EQ(cpu(1).reg(arm64_register::x0), 1u);
    }

    TEST_F(Arm64MultiVcpuBackendTest, AHookedInstructionSteppedByOneVcpuStaysHookedForItsPeer)
    {
        constexpr uint64_t iterations = 300;
        // add x1, x1, #1; subs x2, x2, #1; b.ne -8; svc #1
        load_code(0, std::array{0x91000421u, 0xF1000442u, 0x54FFFFC1u, svc(1)});
        for (size_t i = 0; i < 2; ++i)
        {
            cpu(i).reg(arm64_register::pc, code_address);
            cpu(i).reg(arm64_register::x2, iterations);
        }
        std::array<std::atomic<uint64_t>, 2> hits{};
        machine->hook_memory_execution(code_address, [&](cpu_interface& raw, uint64_t) { ++hits[raw.index()]; });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& raw, uint64_t) {
            raw.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_TRUE(run_both());
        for (size_t i = 0; i < 2; ++i)
        {
            EXPECT_EQ(cpu(i).reg(arm64_register::x1), iterations);
            EXPECT_EQ(hits[i].load(), iterations) << "vCPU " << i << " ran the hooked instruction without the hook";
        }
    }
}
