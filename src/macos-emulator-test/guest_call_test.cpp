#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_guest_call.hpp>
#include <gui/macos_native_dispatch.hpp>

#include <vector>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t hooked_base = 0x100004000ULL;
    constexpr uint64_t adder_base = 0x100008000ULL;
    constexpr uint64_t doubler_base = 0x10000C000ULL;

    sogen::macos_guest_call_stack* g_calls = nullptr;
    uint64_t g_final_result = 0;
    uint64_t g_chain_steps = 0;

    void chaining_handler(const sogen::macos_native_call& call)
    {
        g_chain_steps = 0;

        g_calls->begin(call.emu_ref, sogen::macos_guest_call_request{
                                         .function = adder_base,
                                         .args = {5, 6},
                                         .on_return =
                                             [](sogen::macos_emulator& emu, const uint64_t result) {
                                                 ++g_chain_steps;
                                                 g_calls->begin(emu, sogen::macos_guest_call_request{
                                                                         .function = doubler_base,
                                                                         .args = {result},
                                                                         .on_return =
                                                                             [](sogen::macos_emulator&, const uint64_t final_value) {
                                                                                 ++g_chain_steps;
                                                                                 g_final_result = final_value;
                                                                             },
                                                                     });
                                             },
                                     });
    }

    TEST(GuestCall, ChainsTwoGuestCallsAndReturnsToTheOriginalCaller)
    {
        g_final_result = 0;
        g_chain_steps = 0;

        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        g_calls = &calls;

        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const std::vector<uint32_t> adder{0x8B010000, 0xD65F03C0}; // add x0, x0, x1 ; ret
        emu->memory.write_memory(adder_base, adder.data(), adder.size() * sizeof(uint32_t));

        ASSERT_TRUE(emu->memory.allocate_memory(doubler_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const std::vector<uint32_t> doubler{0x8B000000, 0xD65F03C0}; // add x0, x0, x0 ; ret
        emu->memory.write_memory(doubler_base, doubler.data(), doubler.size() * sizeof(uint32_t));

        ASSERT_TRUE(emu->memory.allocate_memory(hooked_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const uint32_t ret = 0xD65F03C0;
        emu->memory.write_memory(hooked_base, &ret, sizeof(ret));

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0x94000000, // bl hooked_base, patched below
                                         0xD2800030, // mov x16, #1
                                         0xD4001001, // svc #0x80  (exit with x0 from the call)
                                     });

        const auto displacement = static_cast<uint32_t>((hooked_base - code_base) / 4) & 0x03FFFFFFu;
        const uint32_t bl_word = 0x94000000u | displacement;
        emu->memory.write_memory(code_base, &bl_word, sizeof(bl_word));

        sogen::macos_native_dispatch dispatch{};
        dispatch.bind_entry(hooked_base, "ChainProbe", chaining_handler);
        ASSERT_TRUE(sogen::patch_native_entry(*emu, hooked_base));

        emu->set_native_dispatch(&dispatch);
        emu->set_guest_call_stack(&calls);
        emu->start();

        EXPECT_EQ(g_chain_steps, 2u);
        EXPECT_EQ(g_final_result, 22u) << "(5 + 6) doubled, computed by guest code the handler called";
        EXPECT_EQ(emu->process.exit_status, 22);
        EXPECT_FALSE(calls.active());
        EXPECT_EQ(calls.depth(), 0u);
    }

    TEST(GuestCall, TrapPageIsMappedAndFilledWithSvc)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));

        uint32_t word = 0;
        emu->memory.read_memory(sogen::MACOS_GUI_TRAP_BASE, &word, sizeof(word));
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);

        emu->memory.read_memory(sogen::MACOS_GUI_TRAP_BASE + sogen::MACOS_PAGE_SIZE - 4, &word, sizeof(word));
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);

        EXPECT_TRUE(calls.is_trap(sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_TRUE(calls.is_trap(sogen::MACOS_GUI_TRAP_BASE + 4)) << "the whole page traps, not just its first word";
        EXPECT_TRUE(calls.is_trap(sogen::MACOS_GUI_TRAP_BASE + sogen::MACOS_PAGE_SIZE - 4));
        EXPECT_FALSE(calls.is_trap(sogen::MACOS_GUI_TRAP_BASE - 4));
        EXPECT_FALSE(calls.is_trap(sogen::MACOS_GUI_TRAP_BASE + sogen::MACOS_PAGE_SIZE));
    }

    TEST(GuestCall, RefusesToRecurseBeyondTheDepthLimit)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));

        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        size_t accepted = 0;
        for (size_t i = 0; i < 32; ++i)
        {
            if (calls.begin(*emu, sogen::macos_guest_call_request{.function = adder_base}))
            {
                ++accepted;
            }
        }

        EXPECT_EQ(accepted, sogen::MACOS_GUI_MAX_CALL_DEPTH);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(GuestCall, RejectsCallsToUnmappedFunctions)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));

        EXPECT_FALSE(calls.begin(*emu, sogen::macos_guest_call_request{.function = 0}));
        EXPECT_FALSE(calls.begin(*emu, sogen::macos_guest_call_request{.function = 0x900000000ULL}));
        EXPECT_FALSE(calls.active());
    }

    // A native handler is entered from guest code, so its own caller is waiting at an address that has
    // nothing to do with any chain already in flight. One return address for the whole stack strands it.
    TEST(GuestCall, EachFrameReturnsToItsOwnCaller)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr uint64_t outer_return = 0x100000010ULL;
        constexpr uint64_t inner_return = 0x100000020ULL;

        emu->emu().reg(sogen::arm64_register::lr, outer_return);
        ASSERT_TRUE(calls.begin(*emu, sogen::macos_guest_call_request{.function = adder_base}));
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::lr), sogen::MACOS_GUI_TRAP_BASE);

        emu->emu().reg(sogen::arm64_register::lr, inner_return);
        ASSERT_TRUE(calls.begin(*emu, sogen::macos_guest_call_request{.function = adder_base}));
        EXPECT_EQ(calls.depth(), 2u);

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(calls.depth(), 1u);
        EXPECT_EQ(emu->emu().read_instruction_pointer(), inner_return);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::lr), inner_return);

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(calls.depth(), 0u);
        EXPECT_EQ(emu->emu().read_instruction_pointer(), outer_return);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::lr), outer_return);
    }

    // A chain started while the guest is parked on its own svc has no call site to return to: pc has
    // been rewound onto the svc and the trap's arguments are still in the registers the chain is about
    // to use for its own.
    TEST(GuestCall, AnArmedResumePutsTheParkedGuestBackWhenTheChainEmpties)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        ASSERT_TRUE(emu->memory.allocate_memory(doubler_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr uint64_t parked_pc = 0x100000010ULL;
        constexpr uint64_t parked_lr = 0x100000020ULL;
        constexpr uint64_t parked_sp = 0x300000800ULL;

        auto& backend = emu->emu();
        backend.reg(sogen::arm64_register::pc, parked_pc);
        backend.reg(sogen::arm64_register::lr, parked_lr);
        backend.reg(sogen::arm64_register::sp, parked_sp);
        backend.reg(sogen::arm64_register::x0, 0x1111);
        backend.reg(sogen::arm64_register::x16, 0xFFFFFFFFFFFFFFB0ULL);
        backend.reg(sogen::arm64_register::x28, 0x2222);

        ASSERT_TRUE(calls.arm_resume(*emu));
        ASSERT_TRUE(calls.begin(
            *emu,
            sogen::macos_guest_call_request{
                .function = adder_base,
                .args = {7, 8},
                .on_return = [&](sogen::macos_emulator& inner,
                                 uint64_t) { calls.begin(inner, sogen::macos_guest_call_request{.function = doubler_base, .args = {9}}); },
            }));

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(calls.depth(), 1u);
        EXPECT_EQ(backend.read_instruction_pointer(), doubler_base) << "the park comes back only once the chain has emptied";

        backend.reg(sogen::arm64_register::x0, 0xDEAD);
        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));

        EXPECT_EQ(calls.depth(), 0u);
        EXPECT_EQ(backend.read_instruction_pointer(), parked_pc) << "resuming re-runs the svc the guest parked on";
        EXPECT_EQ(backend.reg(sogen::arm64_register::lr), parked_lr);
        EXPECT_EQ(backend.reg(sogen::arm64_register::sp), parked_sp);
        EXPECT_EQ(backend.reg(sogen::arm64_register::x0), 0x1111u) << "the trap's own arguments, not the chain's result";
        EXPECT_EQ(backend.reg(sogen::arm64_register::x16), 0xFFFFFFFFFFFFFFB0ULL) << "the trap number";
        EXPECT_EQ(backend.reg(sogen::arm64_register::x28), 0x2222u);
    }

    TEST(GuestCall, ArmingAResumeIsRefusedWhileAChainIsInFlight)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        ASSERT_TRUE(calls.begin(*emu, sogen::macos_guest_call_request{.function = adder_base}));
        EXPECT_FALSE(calls.arm_resume(*emu)) << "the snapshot would be of a callee, not of the interrupted guest";
    }

    TEST(GuestCall, ADisarmedResumeLeavesTheOrdinaryReturnPath)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr uint64_t caller_return = 0x100000010ULL;
        emu->emu().reg(sogen::arm64_register::pc, 0x100000040ULL);

        ASSERT_TRUE(calls.arm_resume(*emu));
        calls.disarm_resume();

        emu->emu().reg(sogen::arm64_register::lr, caller_return);
        ASSERT_TRUE(calls.begin(*emu, sogen::macos_guest_call_request{.function = adder_base}));
        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));

        EXPECT_EQ(emu->emu().read_instruction_pointer(), caller_return);
    }

    // A callee compiled for arm64e returns with `retab`, which authenticates x30 and branches but leaves
    // the signature in the register. So the lr a continuation reads back is the trap page with a
    // signature above it -- measured 0x003C0002F0000000 for a trap page at 0x2F0000000, running the
    // guest's own CoreFoundation. Comparing it unmasked makes a continuation look like a fresh caller
    // and the chain returns into the signature.
    TEST(GuestCall, RecognisesAPacSignedTrapReturn)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        ASSERT_TRUE(emu->memory.allocate_memory(doubler_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr uint64_t caller_return = 0x100000010ULL;
        constexpr uint64_t pac_signature = 0x003C000000000000ULL;

        emu->emu().reg(sogen::arm64_register::lr, caller_return);
        ASSERT_TRUE(calls.begin(
            *emu, sogen::macos_guest_call_request{
                      .function = adder_base,
                      .on_return = [&](sogen::macos_emulator& inner,
                                       uint64_t) { calls.begin(inner, sogen::macos_guest_call_request{.function = doubler_base}); },
                  }));

        emu->emu().reg(sogen::arm64_register::lr, pac_signature | sogen::MACOS_GUI_TRAP_BASE);
        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(calls.depth(), 1u);

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(emu->emu().read_instruction_pointer(), caller_return);
    }

    // A continuation runs after its own frame has been popped, with lr still pointing at the trap page,
    // so a call it starts has to inherit the address that frame carried rather than read lr.
    TEST(GuestCall, AContinuationInheritsTheReturnAddressOfTheFrameItPopped)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_guest_call_stack calls{};
        ASSERT_TRUE(calls.prepare(*emu));
        ASSERT_TRUE(emu->memory.allocate_memory(adder_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        ASSERT_TRUE(emu->memory.allocate_memory(doubler_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        constexpr uint64_t caller_return = 0x100000010ULL;
        emu->emu().reg(sogen::arm64_register::lr, caller_return);

        bool chained = false;
        ASSERT_TRUE(calls.begin(*emu, sogen::macos_guest_call_request{
                                          .function = adder_base,
                                          .on_return =
                                              [&](sogen::macos_emulator& inner, uint64_t) {
                                                  chained = true;
                                                  calls.begin(inner, sogen::macos_guest_call_request{.function = doubler_base});
                                              },
                                      }));

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_TRUE(chained);
        EXPECT_EQ(calls.depth(), 1u);
        EXPECT_EQ(emu->emu().read_instruction_pointer(), doubler_base) << "the continuation's own call runs before any return";

        ASSERT_TRUE(calls.handle_trap(*emu, sogen::MACOS_GUI_TRAP_BASE));
        EXPECT_EQ(calls.depth(), 0u);
        EXPECT_EQ(emu->emu().read_instruction_pointer(), caller_return);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::lr), caller_return);
    }
}
