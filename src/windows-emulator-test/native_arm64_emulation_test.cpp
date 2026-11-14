#include "emulation_test_utils.hpp"

#include <cstdlib>

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
        settings.intercept_kusd = false;
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
