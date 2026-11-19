#include "std_include.hpp"

#include <cpu_context.hpp>
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

        // fnstenv [rdi]: the 28-byte environment, with the full tag word at offset 8.
        constexpr std::array<uint8_t, 2> store_environment{0xD9, 0x37};
        constexpr uint64_t environment_tag_word = 8;
        constexpr uint16_t default_control_word = 0x37F;
        constexpr uint16_t top_is_register_7 = 7 << 11;

        // 1.0 as an x87 register: the integer bit and the biased exponent of 2^0.
        constexpr M128A one{.Low = 0x8000000000000000, .High = 0x3FFF};

        size_t gdb_register_index(const x86_register reg)
        {
            const auto entry = std::ranges::find(x64_gdb_registers, static_cast<int>(reg), &register_entry::reg);
            return static_cast<size_t>(entry - x64_gdb_registers.begin());
        }
    }

    // FXSAVE's abridged tag word, the form every backend with x87 registers gives: one bit for each physical register, set
    // while it holds a value.
    TEST(X87TagWordTest, TheTagWordHasOneBitForEachRegisterInUse)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        if (!cpu.supports_x87_registers())
        {
            GTEST_SKIP() << "The " << emu.emu().get_name() << " backend has no x87 registers";
        }
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
        if (!cpu.supports_x87_registers())
        {
            GTEST_SKIP() << "The " << emu.emu().get_name() << " backend has no x87 registers";
        }
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

    // NtContinue and NtSetContextThread restore the x87 registers before the tag word, so a backend that keeps FSAVE's
    // full form classifies the values the context brings, not the ones it replaces.
    TEST(X87TagWordTest, ARestoredContextTagsTheValuesItBrings)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        if (!cpu.supports_x87_registers())
        {
            GTEST_SKIP() << "The " << emu.emu().get_name() << " backend has no x87 registers";
        }
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, store_environment.data(), store_environment.size());
        const auto environment = emu.memory.allocate_memory(0x1000, memory_permission::read_write);

        // ST(0) is physical register 7, the only one in use: zero first, then 1.0.
        CONTEXT64 context{};
        context.ContextFlags = CONTEXT_FLOATING_POINT_64;
        context.FltSave.ControlWord = default_control_word;
        context.FltSave.StatusWord = top_is_register_7;
        context.FltSave.TagWord = static_cast<BYTE>(only_register_7_in_use);
        cpu_context::restore(cpu, context);
        context.FltSave.FloatRegisters[0] = one;
        cpu_context::restore(cpu, context);

        cpu.reg(x86_register::rip, code);
        cpu.reg(x86_register::rdi, environment);
        emu.start(1);
        ASSERT_EQ(code + store_environment.size(), cpu.read_instruction_pointer());
        EXPECT_EQ(only_register_7_valid, emu.memory.read_memory<uint16_t>(environment + environment_tag_word));
    }
}
