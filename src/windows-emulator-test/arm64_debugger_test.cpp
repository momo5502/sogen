#include "emulation_test_utils.hpp"

#include <debug_session.hpp>

#include <array>
#include <cstdlib>

namespace sogen::test
{
    TEST(Arm64DebuggerTest, InspectsAnArm64ThreadAtTheLoaderEntry)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_settings settings{};
        settings.emulation_root = root;
        application_settings app_settings{};
        app_settings.application = "C:\\hello_crt.exe";

        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();

        const auto machine = get_application_machine(settings, app_settings);
        windows_emulator emu{
            create_emulator_for_machine(get_machine_type(machine)), std::move(app_settings), settings, {}, std::move(interfaces)};
        emu.log.disable_output(true);
        if (!emu.emu().supports_global_memory_execution_hooks())
        {
            GTEST_SKIP() << "The debugger needs global execution hooks";
        }

        emu.setup_process_if_necessary();
        const debugger::debug_session session{emu};

        // A new thread starts at ntdll!LdrInitializeThunk: pacibsp; str; stp; mov; mov; bl ...
        const auto pc = session.instruction_pointer();
        ASSERT_NE(emu.mod_manager.ntdll, nullptr);
        EXPECT_EQ(pc, emu.mod_manager.ntdll->find_export("LdrInitializeThunk"));

        const auto registers = session.registers();
        ASSERT_EQ(registers.size(), 34u);
        EXPECT_EQ(registers.front().name, "x0");
        EXPECT_EQ(registers[29].name, "fp");
        EXPECT_EQ(registers[32].name, "pc");
        EXPECT_EQ(registers[32].value, pc);
        EXPECT_EQ(registers[33].name, "cpsr");

        const auto instructions = session.disassemble(pc, 6);
        ASSERT_EQ(instructions.size(), 6u);
        for (size_t i = 0; i < instructions.size(); ++i)
        {
            EXPECT_EQ(instructions[i].address, pc + i * 4);
            EXPECT_EQ(instructions[i].bytes.size(), 4u);
        }
        EXPECT_EQ(instructions[0].mnemonic, "pacibsp");
        EXPECT_FALSE(instructions[4].is_call);
        EXPECT_TRUE(instructions[5].is_call);
        ASSERT_TRUE(instructions[5].branch.has_value());
        EXPECT_TRUE(emu.mod_manager.ntdll->contains(*instructions[5].branch));

        const auto frames = session.call_stack();
        ASSERT_FALSE(frames.empty());
        EXPECT_EQ(frames.front().instruction_pointer, pc);
    }

    TEST(Arm64DebuggerTest, ClassifiesControlFlowFromCapstoneInstructionGroups)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator emu{create_arm64_emulator(),
                             emulator_settings{.disable_logging = true, .emulation_root = root, .load_registry = false},
                             {},
                             std::move(interfaces)};
        if (!emu.emu().supports_global_memory_execution_hooks())
        {
            GTEST_SKIP() << "The debugger needs global execution hooks";
        }

        // bl +8; blr x1; svc #1; brk #0xf000; tbl v0.16b, {v1.16b}, v2.16b; tbz w0, #0, +8; cbz x0, +8; b.eq +8; br x16; ret
        constexpr std::array<uint32_t, 10> code{0x94000002, 0xD63F0020, 0xD4000021, 0xD43E0000, 0x4E020020,
                                                0x36000040, 0xB4000040, 0x54000040, 0xD61F0200, 0xD65F03C0};
        const auto address = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(address, 0u);
        emu.memory.write_memory(address, code);

        const debugger::debug_session session{emu};
        const auto instructions = session.disassemble(address, code.size());
        ASSERT_EQ(instructions.size(), code.size());

        struct classification
        {
            bool is_call{};
            bool is_jump{};
            bool is_return{};
            std::optional<uint64_t> branch{};
        };

        const std::array<classification, code.size()> expected{{
            {.is_call = true, .branch = address + 8},
            {.is_call = true},
            {},
            {},
            {},
            {.is_jump = true, .branch = address + 0x14 + 8},
            {.is_jump = true, .branch = address + 0x18 + 8},
            {.is_jump = true, .branch = address + 0x1C + 8},
            {.is_jump = true},
            {.is_return = true},
        }};

        for (size_t i = 0; i < code.size(); ++i)
        {
            SCOPED_TRACE(instructions[i].mnemonic);
            EXPECT_EQ(instructions[i].is_call, expected[i].is_call);
            EXPECT_EQ(instructions[i].is_jump, expected[i].is_jump);
            EXPECT_EQ(instructions[i].is_return, expected[i].is_return);
            EXPECT_EQ(instructions[i].branch, expected[i].branch);
        }
    }
}
