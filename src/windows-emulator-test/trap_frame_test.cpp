#include "emulation_test_utils.hpp"
#include <cpu_context.hpp>
#include <syscall_utils.hpp>
#include <thread_context.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
    NTSTATUS handle_NtSetContextThread(const syscall_context& c, handle thread_handle, emulator_object<CONTEXT64> thread_context);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t eflags_reads_as_one = 0x2;
        constexpr uint32_t written_eflags = 0x246; // IF, ZF, PF and the bit that reads as one
        constexpr uint64_t breakpoint = 0x10000;
        constexpr uint64_t dr7_l0 = 0x1;
        constexpr uint64_t dr7_reads_as_one = 0x400;
        constexpr uint64_t all_ones = ~uint64_t{0};
        constexpr uint64_t thread_stack_size = 0x10000;

        // The sample's process: its main thread loaded on vCPU 0, and a second thread that has never run.
        struct sample_threads
        {
            explicit sample_threads(windows_emulator& emulator)
                : emu(emulator)
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->other =
                    this->emu.process.create_thread(this->emu.memory, this->emu.process.rtl_user_thread_start, 0, thread_stack_size, 0);
            }

            syscall_context context() const
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            // The handlers run under the kernel lock, as the syscall dispatcher holds it while it calls them.
            CONTEXT64 read(const handle thread) const
            {
                const emulator_object<CONTEXT64> buffer{this->emu.memory, this->storage};
                CONTEXT64 value{};
                value.ContextFlags = CONTEXT_CONTROL_64 | CONTEXT_DEBUG_REGISTERS_64;
                buffer.write(value);
                EXPECT_EQ(STATUS_SUCCESS,
                          this->emu.synchronize([&] { return syscalls::handle_NtGetContextThread(this->context(), thread, buffer); }));
                return buffer.read();
            }

            void write(const handle thread, const CONTEXT64& value) const
            {
                const emulator_object<CONTEXT64> buffer{this->emu.memory, this->storage};
                buffer.write(value);
                EXPECT_EQ(STATUS_SUCCESS,
                          this->emu.synchronize([&] { return syscalls::handle_NtSetContextThread(this->context(), thread, buffer); }));
            }

            windows_emulator& emu;
            uint64_t storage{};
            handle other{};
        };
    }

    TEST(TrapFrameTest, ANewThreadShowsTheFrameItWasCreatedWith)
    {
        auto emu = create_sample_emulator();
        const sample_threads threads{emu};

        const auto context = threads.read(threads.other);
        EXPECT_EQ(cpu_context::eflags_interrupt, context.EFlags);
        EXPECT_EQ(0u, context.Dr6);
        EXPECT_EQ(0u, context.Dr7);
    }

    TEST(TrapFrameTest, AThreadShowsWhatAWriteLeftInItsFrameUntilItRuns)
    {
        auto emu = create_sample_emulator();
        const sample_threads threads{emu};

        auto context = threads.read(threads.other);
        context.EFlags = written_eflags;
        context.Dr0 = breakpoint;
        context.Dr6 = all_ones;
        context.Dr7 = dr7_l0 | dr7_reads_as_one;
        threads.write(threads.other, context);

        const auto written = threads.read(threads.other);
        EXPECT_EQ((written_eflags & cpu_context::eflags_user_legal) | cpu_context::eflags_interrupt, written.EFlags);
        EXPECT_EQ(breakpoint, written.Dr0);
        EXPECT_EQ(0u, written.Dr6);
        EXPECT_EQ(dr7_l0, written.Dr7);
    }

    // A write through the process target, such as a debugger's, comes from no thread of the process: the thread loaded
    // on a vCPU keeps it in its frame as well, since it has not returned to user mode either.
    TEST(TrapFrameTest, TheLoadedThreadShowsWhatAHostWriteLeftInItsFrame)
    {
        auto emu = create_sample_emulator();
        const sample_threads threads{emu};
        auto& thread = *emu.vcpu(0).active_thread;

        auto context = threads.read(CURRENT_THREAD);
        context.EFlags = written_eflags;
        context.Dr6 = all_ones;
        context.Dr7 = dr7_l0 | dr7_reads_as_one;
        std::vector<uint8_t> bytes(sizeof(context));
        memcpy(bytes.data(), &context, sizeof(context));
        ASSERT_TRUE(emu.synchronize([&] { return apply_thread_context(emu, emu.vcpu(0), thread, bytes); }));

        const auto captured = emu.synchronize([&] { return capture_thread_context(emu, emu.vcpu(0), thread); });
        ASSERT_EQ(sizeof(CONTEXT64), captured.size());
        CONTEXT64 written{};
        memcpy(&written, captured.data(), sizeof(written));
        EXPECT_EQ((written_eflags & cpu_context::eflags_user_legal) | cpu_context::eflags_interrupt, written.EFlags);
        EXPECT_EQ(0u, written.Dr6);
        EXPECT_EQ(dr7_l0, written.Dr7);
    }

    TEST(TrapFrameTest, AThreadShowsWhatTheProcessorHoldsOnceItRan)
    {
        auto emu = create_sample_emulator();
        const sample_threads threads{emu};
        EXPECT_EQ(cpu_context::eflags_interrupt, threads.read(CURRENT_THREAD).EFlags);

        emu.start(1);
        EXPECT_EQ(eflags_reads_as_one, threads.read(CURRENT_THREAD).EFlags & eflags_reads_as_one);
    }

    // A thread that writes its own context returns to user mode with it right away: what it reads next is what the
    // processor holds, not a frame.
    TEST(TrapFrameTest, AThreadThatWritesItsOwnContextShowsWhatTheProcessorHolds)
    {
        auto emu = create_sample_emulator();
        const sample_threads threads{emu};
        emu.start(1); // a thread makes system calls once it runs

        auto context = threads.read(CURRENT_THREAD);
        context.EFlags = written_eflags;
        threads.write(CURRENT_THREAD, context);
        EXPECT_EQ(eflags_reads_as_one, threads.read(CURRENT_THREAD).EFlags & eflags_reads_as_one);
    }
}
