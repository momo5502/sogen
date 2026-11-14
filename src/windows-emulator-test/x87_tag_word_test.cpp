#include "emulation_test_utils.hpp"

#include <win_gdb_stub_handler.hpp>

namespace sogen::test
{
    namespace
    {
        // fninit; fld1; fld1; fnstsw ax
        constexpr std::array<uint8_t, 8> two_loads{0xDB, 0xE3, 0xD9, 0xE8, 0xD9, 0xE8, 0xDF, 0xE0};
        constexpr uint64_t first_load_end = 4;
        constexpr uint64_t code_end = 8;
        constexpr uint16_t only_register_7_in_use = 0x80;
        constexpr uint16_t only_register_6_in_use = 0x40;
        constexpr uint16_t all_registers_empty = 0;
        constexpr uint16_t invalid_operation_and_stack_fault = 0x41; // FSW.IE, FSW.SF

        // fninit; fld1; fldz; fld1; fdiv st(0), st(1): ST(0) is +infinity in R5, ST(1) zero in R6 and ST(2) 1.0 in R7.
        constexpr std::array<uint8_t, 10> infinity_zero_and_one{0xDB, 0xE3, 0xD9, 0xE8, 0xD9, 0xEE, 0xD9, 0xE8, 0xD8, 0xF1};
        constexpr size_t infinity_zero_and_one_instructions = 5;

        // Two bits for each physical register: R0-R4 empty (11b), R5 special (10b), R6 zero (01b) and R7 valid (00b).
        constexpr uint32_t r5_special_r6_zero_r7_valid = 0x1BFF;
        constexpr uint32_t only_register_7_valid = 0x3FFF;

        size_t gdb_register_index(const x86_register reg)
        {
            const auto entry = std::ranges::find(x64_gdb_registers, static_cast<int>(reg), &register_entry::reg);
            return static_cast<size_t>(entry - x64_gdb_registers.begin());
        }
    }

    // FXSAVE's abridged tag word, the form every backend gives: one bit for each physical register, set while it holds a
    // value.
    TEST(X87TagWordTest, TheTagWordHasOneBitForEachRegisterInUse)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, two_loads.data(), two_loads.size());
        cpu.reg(x86_register::rip, code);

        // The first load goes to physical register 7.
        emu.start(1);
        emu.start(1);
        ASSERT_EQ(code + first_load_end, cpu.read_instruction_pointer());
        EXPECT_EQ(only_register_7_in_use, cpu.reg<uint16_t>(x86_register::fptag));

        // With every register marked empty, the next load has room.
        cpu.reg<uint16_t>(x86_register::fptag, all_registers_empty);
        emu.start(1);
        emu.start(1);
        ASSERT_EQ(code + code_end, cpu.read_instruction_pointer());
        EXPECT_EQ(0, cpu.reg<uint16_t>(x86_register::ax) & invalid_operation_and_stack_fault);
        EXPECT_EQ(only_register_6_in_use, cpu.reg<uint16_t>(x86_register::fptag));
    }

    // GDB's ftag is FSAVE's full tag word, so the GDB stub rebuilds it from the abridged one and turns a written one back.
    TEST(X87TagWordTest, TheGdbStubGivesTheFullTagWord)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, infinity_zero_and_one.data(), infinity_zero_and_one.size());
        cpu.reg(x86_register::rip, code);
        for (size_t i = 0; i < infinity_zero_and_one_instructions; ++i)
        {
            emu.start(1);
        }
        ASSERT_EQ(code + infinity_zero_and_one.size(), cpu.read_instruction_pointer());

        win_gdb_stub_handler handler{emu};
        const auto ftag = gdb_register_index(x86_register::fptag);

        uint32_t full_tag{};
        ASSERT_EQ(sizeof(full_tag), handler.read_register(ftag, &full_tag, sizeof(full_tag)));
        EXPECT_EQ(r5_special_r6_zero_r7_valid, full_tag);

        full_tag = only_register_7_valid;
        ASSERT_EQ(sizeof(full_tag), handler.write_register(ftag, &full_tag, sizeof(full_tag)));
        EXPECT_EQ(only_register_7_in_use, cpu.reg<uint16_t>(x86_register::fptag));
    }
}
