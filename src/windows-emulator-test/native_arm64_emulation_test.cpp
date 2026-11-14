#include "emulation_test_utils.hpp"

#include <cstdlib>
#include <cstring>

#ifdef SOGEN_NATIVE_ARM64_SAMPLE
namespace sogen::test
{
    namespace
    {
        class NativeArm64BackendTest : public testing::Test
        {
          protected:
            static constexpr uint64_t code_address = 0x10000;
            static constexpr uint64_t stack_address = 0x20000;
            std::unique_ptr<arm64_emulator> machine = create_arm64_emulator(backend_type::native_arm64);
            memory_manager memory{*machine};

            void SetUp() override
            {
                ASSERT_TRUE(memory.allocate_memory(code_address, 0x1000, memory_permission::all));
                ASSERT_TRUE(memory.allocate_memory(stack_address, 0x1000, memory_permission::read_write));
                machine->reg(arm64_register::pc, code_address);
                machine->reg(arm64_register::sp, stack_address + 0xFF0);
            }
        };

        class NativeArm64CurrentElTest : public NativeArm64BackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class NativeArm64Ld1Test : public NativeArm64BackendTest, public testing::WithParamInterface<uint32_t>
        {
        };

        class NativeArm64ByteLaneLoadTest : public NativeArm64BackendTest, public testing::WithParamInterface<uint32_t>
        {
        };
    }

    TEST_P(NativeArm64CurrentElTest, RaisesUndefinedInstructionWithoutChangingGuestState)
    {
        constexpr uint32_t mrs_current_el = 0xD5384240;
        constexpr uint32_t svc = 0xD4000001 | (0x1234 << 5);
        const auto destination = GetParam();
        machine->write_memory(code_address, std::array{mrs_current_el | destination, svc});
        const auto register_id = [](const uint32_t index) {
            if (index < 29)
            {
                return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + index);
            }
            return index == 29 ? arm64_register::x29 : arm64_register::x30;
        };
        for (uint32_t i = 0; i < 31; ++i)
        {
            machine->reg(register_id(i), 0xABC000ull + i);
        }
        machine->reg(arm64_register::nzcv, 0xA0000000u);
        size_t exceptions{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::undefined_instruction));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->read_stack_pointer(), stack_address + 0xFF0);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            for (uint32_t i = 0; i < 31; ++i)
            {
                EXPECT_EQ(machine->reg(register_id(i)), 0xABC000ull + i) << "x" << i;
            }
            machine->reg(arm64_register::pc, code_address + 4);
        });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, const uint64_t service) {
            ++calls;
            EXPECT_EQ(service, 0x1234u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->read_stack_pointer(), stack_address + 0xFF0);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            for (uint32_t i = 0; i < 31; ++i)
            {
                EXPECT_EQ(machine->reg(register_id(i)), 0xABC000ull + i) << "x" << i;
            }
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(exceptions, 1u);
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_FALSE(machine->has_violation());
    }

    INSTANTIATE_TEST_SUITE_P(Destination, NativeArm64CurrentElTest, testing::Values(0u, 16u, 30u, 31u));

    TEST_F(NativeArm64BackendTest, IllegalInstructionsWithoutAnInterruptHookFail)
    {
        machine->write_memory(code_address, std::array{0x00000000u, 0xD4000001u});
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            ++calls;
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        EXPECT_TRUE(machine->has_violation());
    }

    TEST_F(NativeArm64BackendTest, SoftwareStepExecutesOneInstructionAndCanBeRearmed)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, std::array{0xD2800020u, 0x91000400u, 0x91000400u, 0xD4000001u});
        machine->reg(arm64_register::pstate, software_step);
        machine->reg(arm64_register::nzcv, 0x60000000u);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4 * steps);
            EXPECT_EQ(machine->reg(arm64_register::x0), steps);
            EXPECT_EQ(machine->reg(arm64_register::pstate), 0x60000000u);
            if (steps == 1)
            {
                machine->reg(arm64_register::pstate, 0x60000000u | software_step);
            }
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 2u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 3u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 16);
    }

    TEST_F(NativeArm64BackendTest, SoftwareStepUsesTheExecutedBranchTargetAndLinkRegister)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, uint32_t{0x14000000});
        machine->reg(arm64_register::pstate, software_step);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface& cpu, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            cpu.stop();
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "The step must stop before executing the branch target";
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);

        machine->write_memory(code_address, std::array{0x94000002u, 0xD503201Fu, 0xD4000001u});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(arm64_register::pstate, software_step);
        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(steps, 2u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        EXPECT_EQ(machine->reg(arm64_register::x30), code_address + 4);
    }

    TEST_F(NativeArm64BackendTest, FaultingInstructionCancelsThePendingSoftwareStep)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, std::array{0xD5384250u, 0xD4000001u});
        machine->reg(arm64_register::x16, 0x1234u);
        machine->reg(arm64_register::pstate, software_step);
        size_t exceptions{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++exceptions;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::undefined_instruction));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            EXPECT_EQ(machine->reg(arm64_register::x16), 0x1234u);
            machine->reg(arm64_register::pc, code_address + 4);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(exceptions, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
    }

    TEST_F(NativeArm64BackendTest, SteppedSvcRaisesOnlyTheSyscall)
    {
        constexpr uint64_t software_step = 1u << 21;
        machine->write_memory(code_address, uint32_t{0xD4000001 | (0x1234 << 5)});
        machine->reg(arm64_register::pstate, software_step);
        machine->hook_interrupt([](cpu_interface&, int) { ADD_FAILURE() << "SVC must cancel the software-step exception"; });
        size_t calls{};
        machine->hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, const uint64_t service) {
            ++calls;
            EXPECT_EQ(service, 0x1234u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & software_step, 0u);
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(calls, 1u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
    }

    TEST_F(NativeArm64BackendTest, MmioLoadsPreserveArm64RegisterSemantics)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 16> data{0x80, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 8, 9, 10, 11, 12, 13, 14, 15};

        struct load_case
        {
            uint32_t instruction;
            size_t size;
            uint64_t result;
            uint64_t base_offset = 0x38;
        };

        constexpr std::array cases{
            load_case{0x39400020, 1, 0x80},
            load_case{0x39800020, 1, 0xFFFFFFFFFFFFFF80},
            load_case{0x39C00020, 1, 0xFFFFFF80},
            load_case{0x79400020, 2, 0xFF80},
            load_case{0x79800020, 2, 0xFFFFFFFFFFFFFF80},
            load_case{0x79C00020, 2, 0xFFFFFF80},
            load_case{0xB9400020, 4, 0xFDFEFF80},
            load_case{0xB9800020, 4, 0xFFFFFFFFFDFEFF80},
            load_case{0xF9400020, 8, 0xF9FAFBFCFDFEFF80},
            load_case{0xF9400420, 8, 0xF9FAFBFCFDFEFF80, 0x30},
            load_case{0xF85F8020, 8, 0xF9FAFBFCFDFEFF80, 0x40},
            load_case{0xF8626820, 8, 0xF9FAFBFCFDFEFF80},
            load_case{0x88DFFC20, 4, 0xFDFEFF80},
        };
        size_t expected_size{};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(
            mmio_address, 0x1000,
            [&](const uint64_t offset, void* output, const size_t size) {
                ++reads;
                EXPECT_EQ(offset, 0x38u);
                EXPECT_EQ(size, expected_size);
                ASSERT_LE(size, data.size());
                std::memcpy(output, data.data(), size);
            },
            [](uint64_t, const void*, size_t) { ADD_FAILURE() << "MMIO writes are unsupported"; }));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto& load : cases)
        {
            SCOPED_TRACE(load.instruction);
            expected_size = load.size;
            machine->write_memory(code_address, std::array{load.instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x0, ~uint64_t{});
            machine->reg(arm64_register::x1, mmio_address + load.base_offset);
            machine->reg(arm64_register::x2, 0u);
            machine->reg(arm64_register::nzcv, 0xA0000000u);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->reg(arm64_register::x0), load.result);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + load.base_offset);
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
            EXPECT_FALSE(machine->has_violation());
        }
        EXPECT_EQ(reads, cases.size());

        for (const auto instruction : {0x3D400020u, 0x3DC00020u, 0x3CC00020u})
        {
            expected_size = instruction == 0x3D400020u ? 1 : 16;
            std::array<uint8_t, 16> initial{};
            initial.fill(0xFF);
            machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x1, mmio_address + 0x38);
            machine->write_register(arm64_register::q0, initial.data(), initial.size());
            ASSERT_NO_THROW(machine->start());
            std::array<uint8_t, 16> result{};
            machine->read_register(arm64_register::q0, result.data(), result.size());
            auto expected = data;
            std::fill(expected.begin() + expected_size, expected.end(), 0);
            EXPECT_EQ(result, expected);
        }
        EXPECT_EQ(reads, cases.size() + 3);
    }

    TEST_F(NativeArm64BackendTest, IndexedMmioLoadsWriteBackTheBaseAfterReading)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 16> data{0x80, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 8, 9, 10, 11, 12, 13, 14, 15};

        struct load_case
        {
            uint32_t instruction;
            arm64_register base;
            size_t size;
            uint64_t read_offset;
            uint64_t updated_offset;
            uint64_t result;
            bool vector = false;
        };

        constexpr std::array cases{
            load_case{0x78400402, arm64_register::x0, 2, 0x400, 0x400, 0xFF80},
            load_case{0x78408C02, arm64_register::x0, 2, 0x408, 0x408, 0xFF80},
            load_case{0x785F8C02, arm64_register::x0, 2, 0x3F8, 0x3F8, 0xFF80},
            load_case{0x784FF402, arm64_register::x0, 2, 0x400, 0x4FF, 0xFF80},
            load_case{0x78500C02, arm64_register::x0, 2, 0x300, 0x300, 0xFF80},
            load_case{0x785F8402, arm64_register::x0, 2, 0x400, 0x3F8, 0xFF80},
            load_case{0xB8808402, arm64_register::x0, 4, 0x400, 0x408, 0xFFFFFFFFFDFEFF80},
            load_case{0x3CC10C00, arm64_register::x0, 16, 0x410, 0x410, 0, true},
            load_case{0x3CDF0400, arm64_register::x0, 16, 0x400, 0x3F0, 0, true},
            load_case{0xF84107E2, arm64_register::sp, 8, 0x400, 0x410, 0xF9FAFBFCFDFEFF80},
            load_case{0xF85F0FE2, arm64_register::sp, 8, 0x3F0, 0x3F0, 0xF9FAFBFCFDFEFF80},
            load_case{0xF840841F, arm64_register::x0, 8, 0x400, 0x408, 0x1234},
        };
        const load_case* current{};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ++reads;
                                             EXPECT_EQ(offset, current->read_offset);
                                             ASSERT_EQ(size, current->size);
                                             EXPECT_EQ(machine->reg(current->base), mmio_address + 0x400);
                                             std::memcpy(output, data.data(), size);
                                         },
                                         {}));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto& load : cases)
        {
            SCOPED_TRACE(load.instruction);
            current = &load;
            machine->write_memory(code_address, std::array{load.instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::sp, stack_address + 0xFF0);
            machine->reg(load.base, mmio_address + 0x400);
            machine->reg(arm64_register::x2, 0x1234u);
            machine->reg(arm64_register::nzcv, 0xA0000000u);
            ASSERT_NO_THROW(machine->start());
            EXPECT_EQ(machine->reg(load.base), mmio_address + load.updated_offset);
            if (load.vector)
            {
                std::array<uint8_t, 16> result{};
                machine->read_register(arm64_register::q0, result.data(), result.size());
                EXPECT_EQ(result, data);
            }
            else
            {
                EXPECT_EQ(machine->reg(arm64_register::x2), load.result);
            }
            EXPECT_EQ(machine->reg(arm64_register::nzcv), 0xA0000000u);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 8);
        }
        EXPECT_EQ(reads, cases.size());

        current = &cases[1];
        ASSERT_TRUE(memory.release_memory(mmio_address, 0x1000));
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [](uint64_t, void*, size_t) { throw std::runtime_error("MMIO device read failed"); }, {}));
        machine->write_memory(code_address, std::array{current->instruction, 0xD4000001u});
        machine->reg(arm64_register::pc, code_address);
        machine->reg(current->base, mmio_address + 0x400);
        machine->reg(arm64_register::x2, 0x1234u);
        EXPECT_THROW(machine->start(), std::runtime_error);
        EXPECT_EQ(machine->reg(current->base), mmio_address + 0x400);
        EXPECT_EQ(machine->reg(arm64_register::x2), 0x1234u);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address);
    }

    TEST_P(NativeArm64Ld1Test, LoadsOneVectorAndWritesBackAfterEachRead)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 32> data{0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                               16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ASSERT_LT(reads, 2u);
                                             ASSERT_EQ(size, 16u);
                                             EXPECT_EQ(offset, 0x40 + 16 * reads);
                                             EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x40 + 16 * reads);
                                             std::memcpy(output, data.data() + 16 * reads, size);
                                             ++reads;
                                         },
                                         {}));
        const uint32_t instruction = 0x4CDF70A0u | (GetParam() << 10);
        machine->write_memory(code_address, std::array{instruction, instruction, 0xD4000001u});
        machine->reg(arm64_register::x5, mmio_address + 0x40);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x50);
            std::array<uint8_t, 16> vector{};
            machine->read_register(arm64_register::q0, vector.data(), vector.size());
            EXPECT_TRUE(std::equal(vector.begin(), vector.end(), data.begin()));
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 2u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x5), mmio_address + 0x60);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        std::array<uint8_t, 16> vector{};
        machine->read_register(arm64_register::q0, vector.data(), vector.size());
        EXPECT_TRUE(std::equal(vector.begin(), vector.end(), data.begin() + 16));
    }

    INSTANTIATE_TEST_SUITE_P(ElementSize, NativeArm64Ld1Test, testing::Values(0u, 1u, 2u, 3u));

    TEST_P(NativeArm64ByteLaneLoadTest, UpdatesOnlyTheSelectedByteLanesAndWritesBackAfterReading)
    {
        constexpr uint64_t mmio_address = 0x40000;
        constexpr std::array<uint8_t, 8> data{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
        const auto count = GetParam();
        const auto registers = count == 2 ? std::array{31u, 0u, 0u, 0u} : std::array{30u, 31u, 0u, 1u};
        const uint32_t instruction = (count == 2 ? 0x0DFF0020u : 0x0DFF2020u) | registers[0];
        std::array<std::array<uint8_t, 16>, 32> expected{};
        const auto vector_register = [](const uint32_t index) {
            return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::q0) + index);
        };
        for (uint32_t i = 0; i < expected.size(); ++i)
        {
            expected[i].fill(static_cast<uint8_t>(0xA0 + i));
            machine->write_register(vector_register(i), expected[i].data(), expected[i].size());
        }
        const auto expect_vectors = [&](const size_t offset) {
            for (uint32_t i = 0; i < count; ++i)
            {
                expected[registers[i]][0] = data[offset + i];
            }
            for (uint32_t i = 0; i < expected.size(); ++i)
            {
                std::array<uint8_t, 16> vector{};
                machine->read_register(vector_register(i), vector.data(), vector.size());
                EXPECT_EQ(vector, expected[i]) << "Q" << i;
            }
        };
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             ASSERT_LT(reads, 2u);
                                             ASSERT_EQ(size, count);
                                             EXPECT_EQ(offset, 0x40 + count * reads);
                                             EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + count * reads);
                                             std::memcpy(output, data.data() + count * reads, size);
                                             ++reads;
                                         },
                                         {}));
        machine->write_memory(code_address, std::array{instruction, instruction, 0xD4000001u});
        machine->reg(arm64_register::x1, mmio_address + 0x40);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + count);
            expect_vectors(0);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 2u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 0x40 + 2 * count);
        EXPECT_EQ(machine->read_instruction_pointer(), code_address + 12);
        expect_vectors(count);
    }

    INSTANTIATE_TEST_SUITE_P(RegisterCount, NativeArm64ByteLaneLoadTest, testing::Values(2u, 4u));

    TEST_F(NativeArm64BackendTest, EveryMmioLoadReadsTheCallbackIncludingZeroRegisterAndSoftwareStep)
    {
        constexpr uint64_t mmio_address = 0x40000;
        size_t reads{};
        ASSERT_TRUE(memory.allocate_mmio(mmio_address, 0x1000,
                                         [&](const uint64_t offset, void* output, const size_t size) {
                                             EXPECT_EQ(offset, reads == 0 ? 0u : 8u);
                                             ASSERT_EQ(size, sizeof(uint64_t));
                                             const uint64_t value = ++reads;
                                             std::memcpy(output, &value, sizeof(value));
                                         },
                                         {}));
        machine->write_memory(code_address, std::array{0xF8408420u, 0xF940003Fu, 0xF9400022u, 0xD4000001u});
        machine->reg(arm64_register::x1, mmio_address);
        machine->reg(arm64_register::pstate, 1u << 21);
        size_t steps{};
        machine->hook_interrupt([&](cpu_interface&, const int exception_class) {
            ++steps;
            EXPECT_EQ(exception_class, static_cast<int>(arm64_exception_class::software_step));
            EXPECT_EQ(machine->read_instruction_pointer(), code_address + 4);
            EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
            EXPECT_EQ(machine->reg(arm64_register::x1), mmio_address + 8);
            EXPECT_EQ(machine->reg(arm64_register::pstate) & (1u << 21), 0u);
        });
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface& cpu, uint64_t) {
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });

        ASSERT_NO_THROW(machine->start());
        EXPECT_EQ(reads, 3u);
        EXPECT_EQ(steps, 1u);
        EXPECT_EQ(machine->reg(arm64_register::x0), 1u);
        EXPECT_EQ(machine->reg(arm64_register::x2), 3u);
    }

    TEST_F(NativeArm64BackendTest, UnsupportedMmioInstructionsFailWithoutCallingCallbacks)
    {
        constexpr uint64_t mmio_address = 0x40000;
        ASSERT_TRUE(memory.allocate_mmio(
            mmio_address, 0x1000, [](uint64_t, void*, size_t) { ADD_FAILURE() << "Unexpected MMIO read"; },
            [](uint64_t, const void*, size_t) { ADD_FAILURE() << "Unexpected MMIO write"; }));
        machine->hook_instruction(arm64_hookable_instructions::svc, [](cpu_interface&, uint64_t) {
            ADD_FAILURE() << "Execution must stop at the unsupported MMIO instruction";
            return instruction_hook_continuation::skip_instruction;
        });
        for (const auto instruction : {0xF9000020u, 0xA9400820u, 0xC85F7C20u})
        {
            machine->write_memory(code_address, std::array{instruction, 0xD4000001u});
            machine->reg(arm64_register::pc, code_address);
            machine->reg(arm64_register::x1, mmio_address);
            EXPECT_THROW(machine->start(), std::runtime_error);
            EXPECT_EQ(machine->read_instruction_pointer(), code_address);
        }
        constexpr uint64_t value = 1;
        EXPECT_FALSE(machine->try_write_memory(mmio_address, &value, sizeof(value)));
    }

    TEST(NativeArm64EmulationTest, NtWriteFileUsesSharedWindowsProcessAndThreadSetup)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_settings settings{};
        settings.emulation_root = root;
        settings.intercept_rdtsc = false;
        settings.intercept_kusd = true;
        settings.use_instruction_precision = false;
        settings.mounts = {{.host_path = SOGEN_NATIVE_ARM64_SAMPLE,
                            .guest_path = windows_path{"C:\\native-arm64-hello.exe"},
                            .mode = filesystem_mode::read_only}};
        std::string output{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view data) { output.append(data); };
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();

        windows_emulator emu{create_arm64_emulator(backend_type::native_arm64),
                             application_settings{.application = "C:\\native-arm64-hello.exe"}, settings, std::move(callbacks),
                             std::move(interfaces)};
        emu.log.disable_output(!enable_verbose_logging());
        emu.start();

        EXPECT_EQ(emu.process.exit_status, STATUS_SUCCESS);
        EXPECT_EQ(output, "Hello World\n");
    }

    TEST(NativeArm64EmulationTest, IllegalInstructionReachesGuestHandlerAndResumes)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_settings settings{};
        settings.emulation_root = root;
        settings.intercept_rdtsc = false;
        settings.intercept_kusd = false;
        settings.use_instruction_precision = false;
        settings.mounts = {{.host_path = SOGEN_NATIVE_ARM64_ILLEGAL_INSTRUCTION_SAMPLE,
                            .guest_path = windows_path{"C:\\native-arm64-illegal-instruction.exe"},
                            .mode = filesystem_mode::read_only}};
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();

        windows_emulator emu{create_arm64_emulator(backend_type::native_arm64),
                             application_settings{.application = "C:\\native-arm64-illegal-instruction.exe"}, settings,
                             emulator_callbacks{}, std::move(interfaces)};
        emu.log.disable_output(!enable_verbose_logging());
        emu.start();

        EXPECT_EQ(emu.process.exit_status, STATUS_SUCCESS);
    }

    TEST(NativeArm64EmulationTest, SoftwareStepReachesGuestHandlerAfterExactlyOneInstruction)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_settings settings{};
        settings.emulation_root = root;
        settings.intercept_rdtsc = false;
        settings.intercept_kusd = false;
        settings.use_instruction_precision = false;
        settings.mounts = {{.host_path = SOGEN_NATIVE_ARM64_SINGLE_STEP_SAMPLE,
                            .guest_path = windows_path{"C:\\native-arm64-single-step.exe"},
                            .mode = filesystem_mode::read_only}};
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();

        windows_emulator emu{create_arm64_emulator(backend_type::native_arm64),
                             application_settings{.application = "C:\\native-arm64-single-step.exe"}, settings, emulator_callbacks{},
                             std::move(interfaces)};
        emu.log.disable_output(!enable_verbose_logging());
        emu.start();

        EXPECT_EQ(emu.process.exit_status, STATUS_SUCCESS);
    }
}
#endif
