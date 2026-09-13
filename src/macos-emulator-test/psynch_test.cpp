#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

// The sequence words in these tests are the ones this host's libpthread was measured to send and to
// expect back (lldb, 2026-08-28): a mutex waiter at lock sequence 0x102 queued behind a drop at 0x300
// is handed 0x303, a single condition variable wake answers 0x101, and a broadcast over two waiters
// answers 0x201. A wrong answer here is not an error the guest reports -- libpthread compares the
// value against its own word and spins or aborts somewhere else entirely.
namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t futex_word = 0x300000000ULL;
    constexpr uint64_t mutex_address = 0x310000000ULL;
    constexpr uint64_t cv_address = 0x310000040ULL;

    constexpr uint32_t mov_x9_x0 = 0xAA0003E9;

    constexpr uint64_t sys_psynch_mutexwait = 301;
    constexpr uint64_t sys_psynch_mutexdrop = 302;
    constexpr uint64_t sys_psynch_cvbroad = 303;
    constexpr uint64_t sys_psynch_cvsignal = 304;
    constexpr uint64_t sys_psynch_cvwait = 305;
    constexpr uint64_t sys_psynch_cvclrprepost = 312;

    constexpr uint32_t mutex_flags = 0x20A0;
    constexpr uint32_t drop_flags = 0x30A0;

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

    // The call with its result kept in x9 across the exit that follows, so a test can read the answer
    // after the process has already stopped.
    std::vector<uint32_t> call_then_exit(const uint64_t number, const std::vector<uint64_t>& args, const uint16_t exit_code)
    {
        auto words = macos_test::syscall_sequence(static_cast<int64_t>(number), args);
        words.insert(words.end() - 1, mov_x9_x0);
        const auto exit_words = macos_test::syscall_sequence(1, {exit_code});
        words.insert(words.end() - 1, exit_words.begin(), exit_words.end() - 1);
        return words;
    }

    // A thread that makes one call and then parks on a futex word, handing the cpu to whoever the call
    // woke. Without the park the caller would run on and the woken thread would never be scheduled.
    void write_caller_then_park(sogen::macos_emulator& emu, const uint64_t address, const uint64_t number,
                                const std::vector<uint64_t>& args)
    {
        if (!emu.memory.get_region_info(futex_word).has_value())
        {
            emu.memory.allocate_memory(futex_word, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        }

        const uint32_t locked = 0xABCD;
        emu.memory.write_memory(futex_word, &locked, sizeof(locked));

        auto words = macos_test::syscall_sequence(static_cast<int64_t>(number), args);
        words.insert(words.end() - 1, mov_x9_x0);
        words.pop_back(); // no brk between the two calls
        const auto park_words = macos_test::syscall_sequence(515, {sogen::MACOS_UL_COMPARE_AND_WAIT, futex_word, locked, 0});
        words.insert(words.end(), park_words.begin(), park_words.end());
        write_code(emu, address, words);
    }

    // The decisive measurement: three threads queued on one mutex with lock sequences 0x102, 0x202 and
    // 0x302 were each handed 0x303. The answer is the *dropper's* sequence word with EBIT and KBIT, not
    // the waiter's own -- taking the waiter's would have answered 0x103 here.
    TEST(Psynch, AMutexWaiterIsHandedTheDroppersSequenceWord)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t dropper_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, dropper_code);

        write_code(*emu, code_base, call_then_exit(sys_psynch_mutexwait, {mutex_address, 0x102, 0, 0, mutex_flags}, 41));
        write_caller_then_park(*emu, dropper_code, sys_psynch_mutexdrop, {mutex_address, 0x300, 0x100, 0, drop_flags});

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{0x303});
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_psynch_mutex, 0u);
        EXPECT_TRUE(emu->process.psynch_mutex_preposts.empty()) << "the hand-off found its waiter";
    }

    // The hand-off goes to the longest-queued waiter, not to whichever thread the thread map happens to
    // visit first: the thread created second queues first here and must be served first.
    TEST(Psynch, TheLongestQueuedMutexWaiterIsServedFirst)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t second_code = code_base + 0x400;
        constexpr uint64_t exit_code_address = code_base + 0x800;
        const auto second_in_line = spawn_thread(*emu, 0x321000000ULL, code_base);
        const auto first_in_line = spawn_thread(*emu, 0x322000000ULL, second_code);
        spawn_thread(*emu, 0x323000000ULL, exit_code_address);

        write_code(*emu, code_base,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_mutexwait), {mutex_address, 0x302, 0, 0, mutex_flags}));
        write_code(*emu, second_code,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_mutexwait), {mutex_address, 0x202, 0, 0, mutex_flags}));
        write_code(*emu, exit_code_address, macos_test::syscall_sequence(1, {41}));

        ASSERT_TRUE(emu->activate_thread(first_in_line));
        emu->emu().reg(sogen::arm64_register::pc, second_code);
        emu->start();

        ASSERT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        ASSERT_EQ(emu->process.threads.at(first_in_line).blocked_on_psynch_mutex, mutex_address);
        ASSERT_EQ(emu->process.threads.at(second_in_line).blocked_on_psynch_mutex, mutex_address);

        EXPECT_EQ(emu->process.wake_psynch_mutex_waiter_of(mutex_address, 0x403), first_in_line);
        EXPECT_EQ(emu->process.wake_psynch_mutex_waiter_of(mutex_address, 0x503), second_in_line);
        EXPECT_EQ(emu->process.wake_psynch_mutex_waiter_of(mutex_address, 0x603), 0u);
    }

    // xnu keeps a hand-off nobody was waiting for as a prepost (kw_pre_rwwc). Dropping it would leave the
    // contender that made libpthread take the kernel path parked on a lock nobody holds.
    TEST(Psynch, AMutexDropWithNoWaiterIsPrepostedForTheNextWait)
    {
        const auto emu = macos_test::make_emulator();
        const auto thread = spawn_thread(*emu, 0x321000000ULL, code_base);

        auto words = macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_mutexdrop), {mutex_address, 0x300, 0x100, 0, drop_flags});
        words.pop_back();
        const auto wait_words = call_then_exit(sys_psynch_mutexwait, {mutex_address, 0x402, 0, 0, mutex_flags}, 41);
        words.insert(words.end(), wait_words.begin(), wait_words.end());
        write_code(*emu, code_base, words);

        ASSERT_TRUE(emu->activate_thread(thread));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{0x303}) << "the wait consumed the prepost without parking";
        EXPECT_TRUE(emu->process.psynch_mutex_preposts.empty());
    }

    // Nobody left to drop the mutex is a real deadlock, and it has to name itself rather than look like
    // an idle process.
    TEST(Psynch, AMutexWaitWithNothingLeftToDropItIsReportedByName)
    {
        const auto emu = macos_test::make_emulator();
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);

        write_code(*emu, code_base,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_mutexwait), {mutex_address, 0x102, 0, 0, mutex_flags}));

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::psynch_wait_deadlock);
        EXPECT_NE(emu->last_stop_detail().find("psynch_mutexwait on mutex 0x310000000"), std::string::npos) << emu->last_stop_detail();
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_psynch_mutex, 0u);
    }

    // Measured: pthread_cond_signal over one parked waiter is answered 0x101 -- one PTHRW_INC of waiters
    // released, plus the C bit -- and the woken wait itself returns 0.
    TEST(Psynch, ACondSignalReleasesOneWaiterAndAnswersOneIncrement)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t signaller_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        const auto signaller = spawn_thread(*emu, 0x322000000ULL, signaller_code);

        write_code(*emu, code_base, call_then_exit(sys_psynch_cvwait, {cv_address, 0x100000100, 0, 0, 0, 0xA0, 0, 0}, 41));
        write_caller_then_park(*emu, signaller_code, sys_psynch_cvsignal, {cv_address, 0x100, 0, 0, 0, 0, 0, 0});

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0u) << "a woken condition variable wait answers 0";
        EXPECT_EQ(emu->process.threads.at(signaller).saved_regs.x.at(9), uint64_t{0x101});
        EXPECT_TRUE(emu->process.psynch_cv_preposts.empty()) << "the signal found its waiter";
    }

    // Measured: a broadcast over two waiters is answered 0x201. libpthread adds that count to its own S
    // word, so a wake that releases fewer threads than it claims puts the condition variable permanently
    // out of step.
    TEST(Psynch, ABroadcastReleasesEveryClaimedWaiterAndAnswersTheWholeCount)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t second_code = code_base + 0x400;
        constexpr uint64_t broadcaster_code = code_base + 0x800;
        const auto first = spawn_thread(*emu, 0x321000000ULL, code_base);
        const auto second = spawn_thread(*emu, 0x322000000ULL, second_code);
        const auto broadcaster = spawn_thread(*emu, 0x323000000ULL, broadcaster_code);

        write_code(*emu, code_base, call_then_exit(sys_psynch_cvwait, {cv_address, 0x10100000200, 0x100, 0, 0, 0xA0, 0, 0}, 41));
        write_code(*emu, second_code, call_then_exit(sys_psynch_cvwait, {cv_address, 0x10000000300, 0x100, 0, 0, 0xA0, 0, 0}, 42));
        write_caller_then_park(*emu, broadcaster_code, sys_psynch_cvbroad, {cv_address, 0x10000000300, 0x10000000200, 0, 0, 0, 0});

        ASSERT_TRUE(emu->activate_thread(first));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.threads.at(broadcaster).saved_regs.x.at(9), uint64_t{0x201});
        EXPECT_EQ(emu->process.threads.at(first).blocked_on_psynch_cv, 0u);
        EXPECT_EQ(emu->process.threads.at(second).blocked_on_psynch_cv, 0u);
        EXPECT_TRUE(emu->process.psynch_cv_preposts.empty());
    }

    // A broadcast claiming more waiters than are parked preposts the difference and says so with the P
    // bit, which is what eventually sends libpthread to psynch_cvclrprepost.
    TEST(Psynch, ABroadcastThatOutrunsItsWaitersPrepostsTheDifference)
    {
        const auto emu = macos_test::make_emulator();
        const auto broadcaster = spawn_thread(*emu, 0x321000000ULL, code_base);

        write_code(*emu, code_base,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_cvbroad), {cv_address, 0x300, 0x200, 0, 0, 0, 0}));

        ASSERT_TRUE(emu->activate_thread(broadcaster));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), uint64_t{0x203});
        EXPECT_EQ(emu->process.psynch_cv_preposts.at(cv_address), 2u);
    }

    TEST(Psynch, APrepostedWakeIsConsumedByTheNextWaitWithoutParking)
    {
        const auto emu = macos_test::make_emulator();
        const auto thread = spawn_thread(*emu, 0x321000000ULL, code_base);

        auto words = macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_cvsignal), {cv_address, 0x100, 0, 0, 0, 0, 0, 0});
        words.pop_back();
        const auto wait_words = call_then_exit(sys_psynch_cvwait, {cv_address, 0x100000200, 0x100, 0, 0, 0xA0, 0, 0}, 41);
        words.insert(words.end(), wait_words.begin(), wait_words.end());
        write_code(*emu, code_base, words);

        ASSERT_TRUE(emu->activate_thread(thread));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0u);
        EXPECT_TRUE(emu->process.psynch_cv_preposts.empty()) << "the wait consumed the prepost";
    }

    TEST(Psynch, ClrPrepostForgetsThePrepostedWakes)
    {
        const auto emu = macos_test::make_emulator();
        const auto thread = spawn_thread(*emu, 0x321000000ULL, code_base);
        emu->process.psynch_cv_preposts[cv_address] = 3;

        write_code(
            *emu, code_base,
            macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_cvclrprepost), {cv_address, 0x300, 0x100, 0x300, 0, 0x300, 0}));

        ASSERT_TRUE(emu->activate_thread(thread));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start(32);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
        EXPECT_TRUE(emu->process.psynch_cv_preposts.empty());
    }

    // libpthread hands the wait a mutex whenever it could not drop it in userspace, and the kernel drops
    // it as part of the wait. Measured: mugen's low half is the lock sequence, so 0x200 there is what the
    // successor's 0x203 is computed from.
    TEST(Psynch, ACondWaitDropsTheMutexItWasHandedBeforeParking)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t contender_code = code_base + 0x800;
        const auto sleeper = spawn_thread(*emu, 0x321000000ULL, code_base);
        const auto contender = spawn_thread(*emu, 0x322000000ULL, contender_code);

        write_code(*emu, code_base,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_cvwait),
                                                {cv_address, 0x100000100, 0, mutex_address, 0x100000200, 0x10A0, 0, 0}));
        write_code(*emu, contender_code, call_then_exit(sys_psynch_mutexwait, {mutex_address, 0x102, 0, 0, mutex_flags}, 41));

        ASSERT_TRUE(emu->activate_thread(contender));
        emu->emu().reg(sogen::arm64_register::pc, contender_code);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{0x203});
        EXPECT_EQ(emu->process.threads.at(sleeper).blocked_on_psynch_cv, cv_address) << "the wait itself is still parked";
        EXPECT_TRUE(emu->process.psynch_mutex_preposts.empty());
    }

    // The whole park is one call in xnu, so the wake adds nothing: a woken wait must not drop the mutex a
    // second time. Doing so would hand a lock away that the woken thread is about to re-acquire.
    TEST(Psynch, AWokenCondWaitDoesNotDropItsMutexASecondTime)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t signaller_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, signaller_code);

        write_code(*emu, code_base,
                   call_then_exit(sys_psynch_cvwait, {cv_address, 0x100000100, 0, mutex_address, 0x100000200, 0x10A0, 0, 0}, 41));
        write_caller_then_park(*emu, signaller_code, sys_psynch_cvsignal, {cv_address, 0x100, 0, 0, 0, 0, 0, 0});

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << emu->last_stop_detail();
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0u);
        ASSERT_EQ(emu->process.psynch_mutex_preposts.size(), 1u) << "the wait dropped the mutex exactly once";
        EXPECT_EQ(emu->process.psynch_mutex_preposts.at(mutex_address), 0x203u);
    }

    // {sec, nsec} is a relative timeout: libpthread converts an absolute pthread_cond_timedwait deadline
    // itself. With nothing runnable the deadline is the next thing that can happen, and ETIMEDOUT is what
    // lets the wait complete -- this is not a deadlock.
    TEST(Psynch, ATimedCondWaitFiresItsDeadlineWhenNothingElseCanRun)
    {
        const auto emu = macos_test::make_emulator();
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);

        write_code(*emu, code_base, call_then_exit(sys_psynch_cvwait, {cv_address, 0x100000100, 0, 0, 0, 0xA0, 1, 0}, 41));

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        EXPECT_EQ(emu->process.exit_status, 41);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), uint64_t{60}) << "ETIMEDOUT, with the carry flag set";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_psynch_cv, 0u);
    }

    // A {0, 0} timeout is an indefinite wait, not an expired one: that is what every plain
    // pthread_cond_wait sends.
    TEST(Psynch, AZeroTimeoutIsAnIndefiniteWaitAndDeadlocksByName)
    {
        const auto emu = macos_test::make_emulator();
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);

        write_code(*emu, code_base,
                   macos_test::syscall_sequence(static_cast<int64_t>(sys_psynch_cvwait), {cv_address, 0x100000100, 0, 0, 0, 0xA0, 0, 0}));

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::psynch_wait_deadlock);
        EXPECT_NE(emu->last_stop_detail().find("psynch_cvwait on condition variable 0x310000040"), std::string::npos)
            << emu->last_stop_detail();
    }

    // A runnable thread runs before any deadline fires, and a psynch park is never mistaken for a
    // runnable thread by the scheduler.
    TEST(Psynch, RunnableThreadsRunBeforeATimedCondWaitsDeadline)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t other_code = code_base + 0x800;
        const auto waiter = spawn_thread(*emu, 0x321000000ULL, code_base);
        spawn_thread(*emu, 0x322000000ULL, other_code);

        write_code(*emu, code_base, call_then_exit(sys_psynch_cvwait, {cv_address, 0x100000100, 0, 0, 0, 0xA0, 100, 0}, 42));
        write_code(*emu, other_code, macos_test::syscall_sequence(1, {41}));

        ASSERT_TRUE(emu->activate_thread(waiter));
        emu->emu().reg(sogen::arm64_register::pc, code_base);
        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41) << "the runnable thread ran instead of the deadline firing";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_psynch_cv, cv_address);
    }
}
