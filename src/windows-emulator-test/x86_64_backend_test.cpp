#include <backend_selection.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr uint64_t test_range_size = 0x140010000ull;

        // Backends that place guest memory at its host address only accept some ranges, so the memory manager picks
        // one large enough for every test and gives it back for the tests to allocate inside.
        uint64_t find_test_range(memory_manager& memory)
        {
            const auto base = memory.allocate_memory(test_range_size, memory_permission::read_write, true);
            if (!base || !memory.release_memory(base, 0))
            {
                throw std::runtime_error("No guest range for the backend tests");
            }

            return base;
        }

        class X8664BackendTest : public testing::Test
        {
          protected:
            std::unique_ptr<x86_64_emulator> machine = create_x86_64_emulator_from_environment();
            memory_manager memory{*machine};
            const uint64_t base_address = find_test_range(memory);
            const uint64_t code_address = base_address + 0x10000;
            const uint64_t data_address = base_address + 0x20000;
            const uint64_t stack_address = base_address + 0x30000;
            const uint64_t read_only_address = base_address + 0x40000;
            const uint64_t no_execute_address = base_address + 0x50000;
            const uint64_t unmapped_address = base_address + 0x60000;

            void SetUp() override
            {
                ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::read_exec));
                ASSERT_TRUE(memory.allocate_memory(data_address, 0x1000, memory_permission::read_write));
                ASSERT_TRUE(memory.allocate_memory(stack_address, 0x1000, memory_permission::read_write));
                machine->reg(x86_register::rip, code_address);
                machine->reg(x86_register::rsp, stack_address + 0xFF0);
            }

            template <size_t N>
            void load_code(const std::array<uint8_t, N>& code)
            {
                machine->write_memory(code_address, code.data(), code.size());
            }

            void stop_on_syscall()
            {
                machine->hook_instruction(x86_hookable_instructions::syscall, [](cpu_interface& cpu, uint64_t) {
                    cpu.stop();
                    return instruction_hook_continuation::skip_instruction;
                });
            }
        };
    }

    TEST_F(X8664BackendTest, SyscallHookResultsAreVisibleAndExecutionContinuesAfterTheInstruction)
    {
        // mov eax, 0x2C; syscall; mov [rbx], rax; mov eax, 7; syscall
        load_code(
            std::array<uint8_t, 17>{0xB8, 0x2C, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x48, 0x89, 0x03, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x0F, 0x05});
        machine->reg(x86_register::rbx, data_address);
        machine->reg(x86_register::r12, 0x1234u);
        std::vector<uint64_t> services{};
        machine->hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<x86_64_cpu&>(raw);
            EXPECT_EQ(cpu.index(), 0u);
            EXPECT_EQ(cpu.reg(x86_register::r12), 0x1234u);
            services.push_back(cpu.reg<uint32_t>(x86_register::eax));
            if (services.size() == 1)
            {
                cpu.reg(x86_register::rax, 100);
            }
            else
            {
                cpu.stop();
            }
            return instruction_hook_continuation::skip_instruction;
        });

        machine->start();
        EXPECT_EQ(services, (std::vector<uint64_t>{0x2C, 7}));
        EXPECT_EQ(machine->read_memory<uint64_t>(data_address), 100u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 17);
    }

    TEST_F(X8664BackendTest, CpuidHookSuppliesTheResult)
    {
        // mov eax, 0x40000000; cpuid; mov [rdi], rax; mov [rdi+8], rbx; syscall
        load_code(std::array<uint8_t, 16>{0xB8, 0x00, 0x00, 0x00, 0x40, 0x0F, 0xA2, 0x48, 0x89, 0x07, 0x48, 0x89, 0x5F, 0x08, 0x0F, 0x05});
        machine->reg(x86_register::rdi, data_address);
        const auto* hook = machine->hook_instruction(x86_hookable_instructions::cpuid, [](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<x86_64_cpu&>(raw);
            EXPECT_EQ(cpu.reg<uint32_t>(x86_register::eax), 0x40000000u);
            cpu.reg(x86_register::rax, 0x11u);
            cpu.reg(x86_register::rbx, 0x22u);
            cpu.reg(x86_register::rcx, 0x33u);
            cpu.reg(x86_register::rdx, 0x44u);
            return instruction_hook_continuation::skip_instruction;
        });
        if (!hook)
        {
            GTEST_SKIP() << "The " << machine->get_name() << " backend cannot hook cpuid";
        }
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(machine->read_memory<uint64_t>(data_address), 0x11u);
        EXPECT_EQ(machine->read_memory<uint64_t>(data_address + 8), 0x22u);
    }

    TEST_F(X8664BackendTest, BreakpointsAndSoftwareInterruptsReachTheInterruptHook)
    {
        // int3; int 0x29; mov eax, 1; syscall
        load_code(std::array<uint8_t, 10>{0xCC, 0xCD, 0x29, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x05});
        std::vector<int> interrupts{};
        // Backends disagree on whether rip is left at the instruction or after it, so the hook moves it explicitly.
        machine->hook_interrupt([&](cpu_interface& raw, const int interrupt) {
            interrupts.push_back(interrupt);
            dynamic_cast<x86_64_cpu&>(raw).reg(x86_register::rip, code_address + (interrupt == 3 ? 1 : 3));
        });
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(interrupts, (std::vector<int>{3, 0x29}));
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 1u);
    }

    TEST_F(X8664BackendTest, Ud2ReachesTheInvalidInstructionHook)
    {
        // ud2; mov eax, 5; syscall
        load_code(std::array<uint8_t, 9>{0x0F, 0x0B, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x0F, 0x05});
        int invalid{};
        const auto* hook = machine->hook_instruction(x86_hookable_instructions::invalid, [&](cpu_interface& raw, uint64_t) {
            auto& cpu = dynamic_cast<x86_64_cpu&>(raw);
            ++invalid;
            EXPECT_EQ(cpu.read_instruction_pointer(), code_address);
            cpu.reg(x86_register::rip, code_address + 2);
            return instruction_hook_continuation::skip_instruction;
        });
        if (!hook)
        {
            GTEST_SKIP() << "The " << machine->get_name() << " backend cannot hook invalid instructions";
        }
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(invalid, 1);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 5u);
    }

    TEST_F(X8664BackendTest, MemoryFaultsReachTheViolationHookAtTheFaultingInstruction)
    {
        ASSERT_TRUE(memory.allocate_memory(read_only_address, 0x1000, memory_permission::read));
        ASSERT_TRUE(memory.allocate_memory(no_execute_address, 0x1000, memory_permission::read_write));
        // mov rax, [rcx]; mov [rdx], rax; call r8; mov eax, 9; syscall
        load_code(std::array<uint8_t, 16>{0x48, 0x8B, 0x01, 0x48, 0x89, 0x02, 0x41, 0xFF, 0xD0, 0xB8, 0x09, 0x00, 0x00, 0x00, 0x0F, 0x05});
        machine->reg(x86_register::rcx, unmapped_address + 8);
        machine->reg(x86_register::rdx, read_only_address);
        machine->reg(x86_register::r8, no_execute_address);

        struct fault
        {
            uint64_t address;
            uint64_t rip;
            memory_operation operation;
            memory_violation_type type;
            bool operator==(const fault&) const = default;
        };

        std::vector<fault> faults{};
        machine->hook_memory_violation(
            [&](cpu_interface& raw, const uint64_t address, size_t, const memory_operation operation, const memory_violation_type type) {
                auto& cpu = dynamic_cast<x86_64_cpu&>(raw);
                const auto rip = cpu.read_instruction_pointer();
                faults.push_back({.address = address, .rip = rip, .operation = operation, .type = type});
                if (operation == memory_operation::exec)
                {
                    // Return from the faulting call.
                    const auto sp = cpu.reg(x86_register::rsp);
                    cpu.reg(x86_register::rip, cpu.read_memory<uint64_t>(sp));
                    cpu.reg(x86_register::rsp, sp + 8);
                }
                else
                {
                    cpu.reg(x86_register::rip, rip + 3);
                }
                return memory_violation_continuation::resume;
            });
        bool done{};
        machine->hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface& cpu, uint64_t) {
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
            {.address = unmapped_address + 8,
             .rip = code_address,
             .operation = memory_operation::read,
             .type = memory_violation_type::unmapped},
            {.address = read_only_address,
             .rip = code_address + 3,
             .operation = memory_operation::write,
             .type = memory_violation_type::protection},
            {.address = no_execute_address,
             .rip = no_execute_address,
             .operation = memory_operation::exec,
             .type = memory_violation_type::protection},
        };
        EXPECT_EQ(faults, expected);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 9u);
    }

    TEST_F(X8664BackendTest, ExecutionRangeHooksSeeEachInstructionOnceAndCanBeRemoved)
    {
        // mov eax, 1; add eax, 1; add eax, 1; syscall
        load_code(std::array<uint8_t, 13>{0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x01, 0x83, 0xC0, 0x01, 0x0F, 0x05});
        std::vector<uint64_t> executed{};
        emulator_hook* hook{};
        hook = machine->hook_memory_range_execution(code_address + 5, 6, [&](cpu_interface&, const uint64_t address) {
            executed.push_back(address);
            if (executed.size() == 2)
            {
                machine->delete_hook(hook);
            }
        });
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(executed, (std::vector<uint64_t>{code_address + 5, code_address + 8}));
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 3u);

        executed.clear();
        machine->reg(x86_register::rip, code_address);
        machine->start();
        EXPECT_TRUE(executed.empty());
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 3u);
    }

    TEST_F(X8664BackendTest, ExecutionHooksPreserveFlagsBetweenInstructions)
    {
        // sub rdx, rcx; jb +7; mov eax, 1; syscall; mov eax, 2; syscall
        load_code(std::array<uint8_t, 20>{0x48, 0x2B, 0xD1, 0x72, 0x07, 0xB8, 0x01, 0x00, 0x00, 0x00,
                                          0x0F, 0x05, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x05, 0x90});
        bool stop_at_branch{};
        machine->hook_memory_execution([&](cpu_interface& cpu, const uint64_t address) {
            if (stop_at_branch && address == code_address + 3)
            {
                stop_at_branch = false;
                cpu.stop();
            }
        });
        stop_on_syscall();

        for (const auto [destination, source, expected] : {std::tuple{5u, 3u, 1u}, std::tuple{3u, 5u, 2u}})
        {
            for (const bool stop : {false, true})
            {
                machine->reg(x86_register::rip, code_address);
                machine->reg(x86_register::rdx, uint64_t{destination});
                machine->reg(x86_register::rcx, uint64_t{source});
                stop_at_branch = stop;
                machine->start();
                if (stop)
                {
                    EXPECT_EQ(machine->read_instruction_pointer(), code_address + 3);
                    machine->reg(x86_register::eflags, machine->reg<uint32_t>(x86_register::eflags));
                    machine->start();
                }
                EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), expected);
            }
        }
    }

    TEST_F(X8664BackendTest, ExecutionHooksKeepRepeatedVectorOperationsIntact)
    {
        // pcmpeqb xmm0, xmm0; pmovmskb eax, xmm0; pmovmskb edx, xmm0; syscall
        load_code(std::array<uint8_t, 14>{0x66, 0x0F, 0x74, 0xC0, 0x66, 0x0F, 0xD7, 0xC0, 0x66, 0x0F, 0xD7, 0xD0, 0x0F, 0x05});
        machine->hook_memory_execution([](cpu_interface&, uint64_t) {});
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 0xFFFFu);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::edx), 0xFFFFu);
    }

    TEST_F(X8664BackendTest, DivisionReadsAMemoryDivisor)
    {
        // xor edx, edx; div qword ptr [rcx]; mov r8, rax; mov r9, rdx; mov rax, -7; cqo; idiv qword ptr [rcx]; syscall
        load_code(std::array<uint8_t, 27>{0x31, 0xD2, 0x48, 0xF7, 0x31, 0x49, 0x89, 0xC0, 0x49, 0x89, 0xD1, 0x48, 0xC7, 0xC0,
                                          0xF9, 0xFF, 0xFF, 0xFF, 0x48, 0x99, 0x48, 0xF7, 0x39, 0x0F, 0x05, 0x90, 0x90});
        stop_on_syscall();
        machine->write_memory<uint64_t>(data_address, 97);
        machine->reg(x86_register::rcx, data_address);
        machine->reg(x86_register::rax, uint64_t{0x4CDB2E7E9A118D0});

        machine->start();
        EXPECT_EQ(machine->reg<uint64_t>(x86_register::r8), 0x4CDB2E7E9A118D0ull / 97);
        EXPECT_EQ(machine->reg<uint64_t>(x86_register::r9), 0x4CDB2E7E9A118D0ull % 97);
        EXPECT_EQ(machine->reg<int64_t>(x86_register::rax), 0);
        EXPECT_EQ(machine->reg<int64_t>(x86_register::rdx), -7);
    }

    TEST_F(X8664BackendTest, SingleStepExecutesOneInstruction)
    {
        // mov eax, 1; add eax, 1; syscall
        load_code(std::array<uint8_t, 10>{0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xC0, 0x01, 0x0F, 0x05});
        if (!machine->supports_instruction_counting())
        {
            GTEST_SKIP() << "The " << machine->get_name() << " backend cannot count instructions";
        }
        machine->start(1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 5);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 1u);
        machine->start(1);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 2u);
    }

    TEST_F(X8664BackendTest, RegistersAliasesAndSavedStateRoundTrip)
    {
        machine->reg(x86_register::rax, 0x1122334455667788ull);
        EXPECT_EQ(machine->reg<uint8_t>(x86_register::ah), 0x77u);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::eax), 0x55667788u);
        machine->reg<uint16_t>(x86_register::ax, 0x1234u);
        EXPECT_EQ(machine->reg(x86_register::rax), 0x1122334455661234ull);

        const std::array<uint64_t, 2> vector{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
        machine->write_register(x86_register::xmm7, vector.data(), sizeof(vector));
        machine->reg<uint32_t>(x86_register::mxcsr, 0x1F80u);
        machine->set_segment_base(x86_register::gs, 0x7FF000000000ull);
        machine->reg(x86_register::r15, 0x55u);

        const auto saved = machine->save_registers();
        const std::array<uint64_t, 2> empty{};
        machine->write_register(x86_register::xmm7, empty.data(), sizeof(empty));
        machine->set_segment_base(x86_register::gs, 0);
        machine->reg(x86_register::r15, 0u);
        machine->restore_registers(saved);

        std::array<uint64_t, 2> observed{};
        machine->read_register(x86_register::xmm7, observed.data(), sizeof(observed));
        EXPECT_EQ(observed, vector);
        EXPECT_EQ(machine->reg<uint32_t>(x86_register::mxcsr), 0x1F80u);
        EXPECT_EQ(machine->get_segment_base(x86_register::gs), 0x7FF000000000ull);
        EXPECT_EQ(machine->reg(x86_register::r15), 0x55u);
    }

    TEST_F(X8664BackendTest, GuestAccessesMemoryThroughItsTranslationsAcrossRuns)
    {
        const uint64_t base = base_address + 0x140000000ull;
        ASSERT_TRUE(memory.allocate_memory(base, 0x2000, memory_permission::read_write));
        // mov rax, [rcx]; mov [rcx+0xFFC], rax; syscall
        load_code(std::array<uint8_t, 12>{0x48, 0x8B, 0x01, 0x48, 0x89, 0x81, 0xFC, 0x0F, 0x00, 0x00, 0x0F, 0x05});
        machine->reg(x86_register::rcx, base);
        machine->write_memory(base, uint64_t{41});
        stop_on_syscall();

        machine->start();
        EXPECT_EQ(machine->read_memory<uint64_t>(base + 0xFFC), 41u);

        ASSERT_TRUE(memory.release_memory(base, 0x2000));
        ASSERT_TRUE(memory.allocate_memory(base, 0x2000, memory_permission::read_write));
        machine->write_memory(base, uint64_t{57});
        machine->reg(x86_register::rip, code_address);
        machine->reg(x86_register::rcx, base);
        machine->start();
        EXPECT_EQ(machine->read_memory<uint64_t>(base + 0xFFC), 57u);
    }
}
