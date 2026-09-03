#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t futex_word = 0x300000000ULL;

    constexpr uint32_t mov_x9_x0 = 0xAA0003E9;  // orr x9, xzr, x0
    constexpr uint32_t mov_x10_x0 = 0xAA0003EA; // orr x10, xzr, x0

    void write_code(sogen::macos_emulator& emu, const uint64_t address, const std::vector<uint32_t>& words)
    {
        if (!emu.memory.get_region_info(address).has_value())
        {
            emu.memory.allocate_memory(address, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        }

        emu.memory.write_memory(address, words.data(), words.size() * sizeof(uint32_t));
    }

    uint64_t spawn_thread(sogen::macos_emulator& emu, const uint64_t stack_base, const uint64_t entry)
    {
        if (!emu.memory.get_region_info(stack_base).has_value())
        {
            emu.memory.allocate_memory(stack_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        }

        return emu.process.create_thread(stack_base, sogen::MACOS_PAGE_SIZE, entry);
    }

    // __semwait_signal with its result kept in x9 across the exit that follows, so a test can tell
    // ETIMEDOUT from success after the process has already stopped.
    std::vector<uint32_t> semwait_then_exit(const std::vector<uint64_t>& args, const uint16_t exit_code)
    {
        auto words = macos_test::syscall_sequence(334, args);
        words.insert(words.end() - 1, mov_x9_x0); // between the svc and the brk
        const auto exit_words = macos_test::syscall_sequence(1, {exit_code});
        words.insert(words.end() - 1, exit_words.begin(), exit_words.end() - 1);
        return words;
    }

    TEST(SemwaitSignal, APositiveCountIsConsumedImmediately)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 1);

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {cond, 0, 0, 0, 0, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_EQ(emu->mach.find_semaphore(cond)->value, 0);
    }

    TEST(SemwaitSignal, AnUnknownSemaphoreNameFails)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 1);

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {0x7777, 0, 0, 0, 0, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{22}); // EINVAL
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);

        // The mutex name is validated too: a bad mutex fails even with a good condition semaphore.
        write_code(*emu, code_base + 0x100, macos_test::syscall_sequence(334, {cond, 0x7777, 0, 0, 0, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base + 0x100);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{22});
        EXPECT_EQ(emu->mach.find_semaphore(cond)->value, 1) << "a failed call consumes nothing";
    }

    // xnu checks the timespec against BAD_MACH_TIMESPEC after the timeout flag says there is one.
    TEST(SemwaitSignal, ABadTimespecFails)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {cond, 0, 1, 1, 0, 1000000000}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{22}); // EINVAL
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
    }

    // A {0,0} timeout is xnu's NOBLOCK: the answer is ETIMEDOUT without ever parking.
    TEST(SemwaitSignal, AZeroTimeoutIsANonblockingPoll)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        ASSERT_TRUE(emu->activate_thread(waiter));

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {cond, 0, 1, 1, 0, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{60}); // ETIMEDOUT
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_sem, 0u) << "a poll does not park";
    }

    // xnu signals the mutex semaphore on every path, including the immediate timeout: that is the whole
    // point of the wait_signal pairing for pthread cond waits.
    TEST(SemwaitSignal, TheMutexSemaphoreIsSignalledAroundTheWait)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);
        const auto mutex = emu->mach.create_semaphore(0, 0);

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {cond, mutex, 1, 1, 0, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{60}); // ETIMEDOUT
        EXPECT_EQ(emu->mach.find_semaphore(mutex)->value, 1) << "the mutex was released around the wait";
    }

    // The virtual-time step: with nothing runnable, a parked timed waiter's deadline is the next thing
    // that happens. The sleeper wakes with ETIMEDOUT and carries on -- and this is not a deadlock.
    TEST(SemwaitSignal, ATimedWaitFiresItsDeadlineWhenNothingElseCanRun)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        ASSERT_TRUE(emu->activate_thread(waiter));

        write_code(*emu, code_base, semwait_then_exit({cond, 0, 1, 1, 3, 0}, 41));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        EXPECT_EQ(emu->process.exit_status, 41);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{60}) << "ETIMEDOUT is what lets sleep(3) complete";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_sem, 0u);
    }

    // semaphore_signal on the condition semaphore wakes the parked waiter, which consumes the count and
    // returns 0 -- the cond_wait wake path.
    TEST(SemwaitSignal, ASignalOnTheCondSemaphoreWakesTheWaiterWithSuccess)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);

        constexpr uint64_t waker_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        const auto waker = spawn_thread(*emu, 0x322000000ULL, waker_code);

        ASSERT_TRUE(emu->memory.allocate_memory(futex_word, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const uint32_t locked = 0xABCD;
        emu->memory.write_memory(futex_word, &locked, sizeof(locked));

        write_code(*emu, code_base, semwait_then_exit({cond, 0, 0, 0, 0, 0}, 41));

        // Signals the semaphore, then parks on a futex word so the waiter gets the cpu back.
        auto words = macos_test::syscall_sequence(-33, {cond}); // semaphore_signal_trap
        words.pop_back();                                       // no brk between the two calls
        const auto park_words = macos_test::syscall_sequence(515, {sogen::MACOS_UL_COMPARE_AND_WAIT, futex_word, locked, 0});
        words.insert(words.end(), park_words.begin(), park_words.end());
        write_code(*emu, waker_code, words);

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0u) << "the wake consumes the signal and succeeds";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_sem, 0u);
        EXPECT_EQ(emu->mach.find_semaphore(cond)->value, 0);
        EXPECT_EQ(emu->process.threads.at(waker).blocked_on_ulock, futex_word);
    }

    // While any thread is runnable it runs first; a parked timed waiter is skipped by the scheduler and
    // its deadline only matters once nothing else can run.
    TEST(SemwaitSignal, RunnableThreadsRunBeforeAnyDeadlineFires)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);

        constexpr uint64_t other_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, other_code);

        write_code(*emu, code_base, semwait_then_exit({cond, 0, 1, 1, 3, 0}, 42));
        write_code(*emu, other_code, macos_test::syscall_sequence(1, {41}));

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << "the runnable thread ran to exit instead of the deadline firing";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_sem, cond) << "the waiter is still parked";
    }

    // With timeout == 0 xnu never inspects the timespec, so garbage tv_sec cannot turn a completed
    // indefinite wait into EINTR.
    TEST(SemwaitSignal, AnIndefiniteWaitIgnoresTheTimespec)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 1);

        write_code(*emu, code_base, macos_test::syscall_sequence(334, {cond, 0, 0, 0, 0x100000000ULL, 0}));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_EQ(emu->mach.find_semaphore(cond)->value, 0);
    }

    // A mutex-paired wait signals the mutex semaphore exactly once, when the wait registers. A parked-
    // then-woken wait re-runs the svc and must not signal it a second time: in xnu the whole thing is
    // one semaphore_wait_signal. The waiter's semaphore_wait is the caller-side mutex re-acquire; it
    // consumes exactly the one signal the park added.
    TEST(SemwaitSignal, AMutexPairedWaitThatParksAndWakesSignalsTheMutexOnce)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);
        const auto mutex = emu->mach.create_semaphore(0, 0);

        constexpr uint64_t waker_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, waker_code);

        ASSERT_TRUE(emu->memory.allocate_memory(futex_word, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const uint32_t locked = 0xABCD;
        emu->memory.write_memory(futex_word, &locked, sizeof(locked));

        // semwait with its result in x9, then semaphore_wait(mutex) with its result in x10, then exit.
        auto words = macos_test::syscall_sequence(334, {cond, mutex, 0, 0, 0, 0});
        words.insert(words.end() - 1, mov_x9_x0);
        words.pop_back(); // no brk between the calls
        auto wait_words = macos_test::syscall_sequence(-36, {mutex});
        wait_words.insert(wait_words.end() - 1, mov_x10_x0);
        words.insert(words.end(), wait_words.begin(), wait_words.end() - 1);
        const auto exit_words = macos_test::syscall_sequence(1, {41});
        words.insert(words.end(), exit_words.begin(), exit_words.end());
        write_code(*emu, code_base, words);

        // Signals the condition semaphore, then parks on a futex word so the waiter gets the cpu back.
        auto waker_words = macos_test::syscall_sequence(-33, {cond}); // semaphore_signal_trap
        waker_words.pop_back();
        const auto park_words = macos_test::syscall_sequence(515, {sogen::MACOS_UL_COMPARE_AND_WAIT, futex_word, locked, 0});
        waker_words.insert(waker_words.end(), park_words.begin(), park_words.end());
        write_code(*emu, waker_code, waker_words);

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0u) << "the woken wait consumes the signal and succeeds";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x10), 0u) << "the re-acquire succeeds on the parked-time signal";
        EXPECT_EQ(emu->mach.find_semaphore(cond)->value, 0);
        EXPECT_EQ(emu->mach.find_semaphore(mutex)->value, 0)
            << "the park signalled the mutex once and the re-acquire consumed it; the wake signalled nothing";
    }

    // With several timed waiters parked and nothing runnable, the earliest deadline fires first.
    TEST(SemwaitSignal, TheEarliestDeadlineFiresFirst)
    {
        const auto emu = macos_test::make_emulator();
        const auto cond = emu->mach.create_semaphore(0, 0);

        constexpr uint64_t short_code = code_base + 0x800;
        const auto long_waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, short_code);

        write_code(*emu, code_base, semwait_then_exit({cond, 0, 1, 1, 100, 0}, 42));
        write_code(*emu, short_code, semwait_then_exit({cond, 0, 1, 1, 1, 0}, 41));

        ASSERT_TRUE(emu->activate_thread(long_waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << "the one-second wait ends before the hundred-second one";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{60});
        EXPECT_EQ(emu->process.threads.at(long_waiter).blocked_on_sem, cond) << "the later deadline is still parked";
    }
}
