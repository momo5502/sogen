#include "emulation_test_utils.hpp"

namespace sogen::test
{
    namespace
    {
        // mov al, byte ptr [rax]: with rax 0, an access violation
        constexpr std::array<uint8_t, 2> load_from_rax{0x8A, 0x00};
        constexpr uint64_t trap_flag = 0x100; // EFLAGS.TF
        constexpr size_t step_limit = 4;
    }

    // An instruction that raises an exception under a step of the backend's own (on WHP a trap flag step for a
    // one-instruction run and an execution hook on its page) ends that step before the exception is dispatched: the
    // context the guest's handlers get holds the guest's own trap flag, and an execution hook where the exception goes
    // arms a step of its own.
    TEST(StepExceptionContextTest, AnExceptionUnderTheBackendsStepEndsTheStepFirst)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).x64_cpu();
        const auto code = emu.memory.allocate_memory(0x1000, memory_permission::all);
        emu.memory.write_memory(code, load_from_rax.data(), load_from_rax.size());

        size_t code_hits = 0;
        size_t dispatcher_hits = 0;
        std::optional<CONTEXT64> dispatched{}; // the context KiUserExceptionDispatcher starts with
        auto* code_hook = emu.emu().hook_memory_execution(code, [&](cpu_interface&, uint64_t) { ++code_hits; });
        auto* dispatcher_hook = emu.emu().hook_memory_execution(emu.process.ki_user_exception_dispatcher, [&](cpu_interface&, uint64_t) {
            ++dispatcher_hits;
            dispatched = emu.memory.read_memory<CONTEXT64>(cpu.reg(x86_register::rsp));
        });
        cpu.reg(x86_register::rax, uint64_t{0});
        cpu.reg(x86_register::rip, code);

        for (size_t steps = 0; steps < step_limit && dispatcher_hits == 0; ++steps)
        {
            EXPECT_NO_THROW(emu.start(1)) << "the dispatcher's hook arms a step of its own";
        }
        ASSERT_TRUE(dispatched.has_value());
        EXPECT_EQ(code, dispatched->Rip);
        EXPECT_EQ(0u, dispatched->EFlags & trap_flag) << "the handlers see the guest's own trap flag";
        EXPECT_EQ(1u, code_hits);
        EXPECT_EQ(1u, dispatcher_hits);
        emu.emu().delete_hook(code_hook);
        emu.emu().delete_hook(dispatcher_hook);
    }
}
