#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

namespace
{
    // A caller that asks for a bounded run and gets control back is entitled to know why. Reaching the
    // bound is the one outcome the emulator can infer without help from the backend: every other way out
    // of the run loop records its own reason on the way past.
    TEST(MacosInstructionLimit, ReachingTheBoundIsReported)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t base = 0x200000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(base, 0x4000, sogen::memory_permission::all));

        constexpr uint32_t code[] = {
            0xD503201F, // nop
            0xD503201F, // nop
            0xD503201F, // nop
            0xD503201F, // nop
        };
        emu->memory.write_memory(base, code, sizeof(code));
        emu->emu().reg(sogen::arm64_register::pc, base);

        emu->start(2);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::instruction_limit);
        EXPECT_EQ(emu->last_stop_detail(), "count=2");
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), base + 8);
    }

    TEST(MacosInstructionLimit, TellsWhetherItIsCountingInstructions)
    {
        const auto emu = macos_test::make_emulator();

        // Native builds install the basic-block hook that maintains the tally; the browser build cannot,
        // because Unicorn compiles that hook into an inline helper whose signature wasm rejects.
#ifdef __EMSCRIPTEN__
        EXPECT_FALSE(emu->counts_executed_instructions());
#else
        EXPECT_TRUE(emu->counts_executed_instructions());
#endif
    }
}
