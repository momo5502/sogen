#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;

    TEST(MacosFaultSuppression, BareBackendStillRaisesOnUndefinedInstruction)
    {
        const auto backend = macos_test::make_backend();
        backend->map_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);

        constexpr uint32_t undefined = 0x00000000; // udf #0
        backend->write_memory(code_base, &undefined, sizeof(undefined));
        backend->reg(sogen::arm64_register::pc, code_base);

        EXPECT_THROW(backend->start(1), std::exception) << "with no UC_HOOK_INTR registered, unicorn still reports the fault; "
                                                           "if this now passes silently the workaround in macos_emulator can be revisited";
    }

    TEST(MacosFaultSuppression, UndefinedInstructionHaltsInsteadOfSpinning)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD503201F, // nop
                                         0x00000000, // udf #0
                                         0xD503201F, // nop
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        EXPECT_NE(emu->last_stop_detail().find("0x100000004"), std::string::npos)
            << "the stop detail must name the faulting pc, got: " << emu->last_stop_detail();
    }

    // A bounded stand-in for the unbounded spin: start(0) is what actually hangs when the catch-all is
    // missing, which would take the whole suite down with it instead of failing.
    TEST(MacosFaultSuppression, TheFaultingInstructionIsNotRetriedUntilTheBudgetRunsOut)
    {
        constexpr size_t generous_budget = 100000;

        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x00000000, // udf #0
                                     });

        emu->start(generous_budget);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        EXPECT_LT(emu->get_executed_instructions(), generous_budget)
            << "a swallowed fault re-enters the same pc until the instruction budget is exhausted";
    }

    TEST(MacosFaultSuppression, BreakpointHaltsInsteadOfLooping)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD4200000, // brk #0
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
    }

    TEST(MacosFaultSuppression, SvcIsNotTreatedAsAFault)
    {
        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800090, // mov x16, #4
                                         0xD4001001, // svc #0x80
                                         0xD503201F, // nop
                                     });

        emu->start(3);

        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), code_base + 12);
    }
}
