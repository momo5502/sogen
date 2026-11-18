#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtRemoveIoCompletionEx(const syscall_context& c, handle io_completion_handle,
                                           emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> io_completion_information,
                                           ULONG count, emulator_object<ULONG> num_entries_removed, emulator_object<LARGE_INTEGER> timeout,
                                           BOOLEAN alertable);
}

namespace sogen::test
{
    namespace
    {
        // KiUserApcDispatcher's unwind data: UWOP_ALLOC_LARGE 0x530, then UWOP_PUSH_MACHFRAME.
        constexpr uint64_t apc_machine_frame_offset = 0x530;
        constexpr uint64_t stack_alignment = 0x10;
        constexpr uint64_t apc_routine = 0x1000;
    }

    // KiInitializeUserApc leaves a MACHINE_FRAME with the interrupted RIP and RSP right under the interrupted stack, at
    // (Rsp - 0x28) & ~0xF, the frame KiUserApcDispatcher starts on 0x530 below it.
    TEST(UserApcFrameTest, TheApcFrameHoldsWhereTheThreadWasInterrupted)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);

        pending_apc apc{};
        apc.apc_routine = apc_routine;
        vcpu.active_thread->pending_apcs.push_back(apc);
        vcpu.active_thread->apc_alertable = true;
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));

        auto& cpu = vcpu.x64_cpu();
        ASSERT_EQ(emu.process.ki_user_apc_dispatcher, cpu.read_instruction_pointer());

        const auto rsp = cpu.read_stack_pointer();
        const auto interrupted = emu.memory.read_memory<CONTEXT64>(rsp);
        const auto machine_frame = emu.memory.read_memory<MACHINE_FRAME>(rsp + apc_machine_frame_offset);
        EXPECT_EQ((interrupted.Rsp - sizeof(MACHINE_FRAME)) & ~(stack_alignment - 1), rsp + apc_machine_frame_offset);
        EXPECT_EQ(interrupted.Rip, machine_frame.Rip);
        EXPECT_EQ(interrupted.Rsp, machine_frame.Rsp);
    }

    // An APC ends NtRemoveIoCompletionEx's alertable wait with KeRemoveQueueEx's one entry, STATUS_USER_APC, and the call
    // stores that count of 1; nothing is written when the wait starts (19041 on the host).
    TEST(UserApcFrameTest, AnApcEndingAnAlertableRemoveIoCompletionExStoresACountOfOne)
    {
        auto emu = create_sample_emulator();
        emu.setup_process_if_necessary();
        auto& vcpu = emu.vcpu(0);
        auto& thread = *vcpu.active_thread;
        const auto port = emu.process.io_completions.store(io_completion{});
        const auto buffer = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const emulator_object<ULONG> removed{emu.memory, buffer};
        removed.write(0xDEADBEEF);

        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        ASSERT_EQ(STATUS_SUCCESS, emu.synchronize([&] {
            return syscalls::handle_NtRemoveIoCompletionEx(context, port, {emu.memory, buffer + 0x100}, 2, removed, {emu.memory}, TRUE);
        }));
        EXPECT_EQ(0xDEADBEEFu, removed.read());

        pending_apc apc{};
        apc.apc_routine = apc_routine;
        thread.pending_apcs.push_back(apc);
        ASSERT_TRUE(emu.perform_thread_switch(vcpu));
        EXPECT_EQ(1u, removed.read());
    }
}
