#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <guest/guest_fd_table.hpp>
#include <macos_kqueue.hpp>

namespace
{
    constexpr uint64_t code_base = 0x310000000ULL;
    constexpr uint64_t data_base = 0x300000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    void run_syscall(sogen::macos_emulator& emu, const uint64_t page, const int64_t number, const std::vector<uint64_t>& args)
    {
        macos_test::write_guest_code(emu, page, macos_test::syscall_sequence(number, args));
        emu.start(64);
    }

    void write_entry(sogen::macos_emulator& emu, const uint64_t address, const sogen::kevent_registration& registration)
    {
        sogen::macos_kevent_qos_entry entry{};
        entry.ident = registration.ident;
        entry.filter = registration.filter;
        entry.flags = registration.flags;
        entry.udata = registration.udata;
        entry.fflags = registration.fflags;
        entry.data = registration.data;
        std::copy(std::begin(registration.ext), std::end(registration.ext), std::begin(entry.ext));
        emu.memory.write_memory(address, &entry, sizeof(entry));
    }

    uint64_t counter_of(sogen::macos_emulator& emu)
    {
        return emu.emu().read_system_register(3, 3, 14, 0, 2);
    }

    uint64_t nanoseconds_in_ticks(sogen::macos_emulator& emu, const uint64_t nanoseconds)
    {
        return nanoseconds / emu.mach.timebase_numer * emu.mach.timebase_denom;
    }

    // The shape every libdispatch timer is registered with, measured 2026-08-28 on the host under lldb
    // for both a main-queue dispatch_after and a repeating dispatch_source: an absolute mach-time
    // deadline, one-shot, with the leeway in ext[1]. libdispatch re-registers it on every expiry, so it
    // never asks the kernel for an interval.
    sogen::kevent_registration dispatch_timer(const uint64_t deadline)
    {
        sogen::kevent_registration registration{};
        registration.ident = 0xFFFFFFFFFFFFFF00ULL;
        registration.filter = sogen::MACOS_EVFILT_TIMER;
        registration.flags = 0x15; // EV_ADD | EV_ENABLE | EV_ONESHOT
        registration.fflags = sogen::MACOS_NOTE_ABSOLUTE | sogen::MACOS_NOTE_LEEWAY | sogen::MACOS_NOTE_MACHTIME;
        registration.data = static_cast<int64_t>(deadline);
        registration.udata = 0x1F9A5BD70ULL;
        registration.ext[1] = 0x1B773F;
        return registration;
    }

    uint64_t result_of(sogen::macos_emulator& emu)
    {
        return emu.emu().reg(sogen::arm64_register::x0);
    }

    bool failed(sogen::macos_emulator& emu)
    {
        return (emu.emu().reg(sogen::arm64_register::nzcv) & carry) != 0;
    }

    uint32_t make_kqueue(sogen::macos_emulator& emu, const uint64_t page)
    {
        run_syscall(emu, page, 362, {});
        return static_cast<uint32_t>(result_of(emu));
    }

    TEST(Kqueue, KqueueReturnsARealDescriptorThatCloses)
    {
        const auto emu = macos_test::make_emulator();

        const auto kq = make_kqueue(*emu, code_base);
        EXPECT_FALSE(failed(*emu));
        EXPECT_GE(kq, 3u) << "stdin, stdout and stderr are taken";

        const auto* entry = emu->process.fds.get(static_cast<int>(kq));
        ASSERT_NE(entry, nullptr) << "a kqueue is a descriptor on macOS, so it lives in the fd table";
        EXPECT_EQ(entry->type, sogen::fd_type::kqueue);
        EXPECT_NE(emu->process.kqueues.find(kq), nullptr);

        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 6, {kq}); // close
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_EQ(emu->process.fds.get(static_cast<int>(kq)), nullptr);
        EXPECT_EQ(emu->process.kqueues.find(kq), nullptr) << "closing the descriptor destroys the kqueue with it";
    }

    TEST(Kqueue, KeventQosRegistersTheChangelistAndReportsWhatItPlaced)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x1;
        registration.filter = -10; // EVFILT_USER
        registration.flags = 0x21; // EV_ADD | EV_CLEAR
        registration.udata = 0xDEAD;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, // kevent_qos
                    {kq, data_base, 1, data_base + 0x800, 16, 0, 0, 0x1});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u) << "the answer is a count of placed events, and nothing is pending";

        const auto* queue = emu->process.kqueues.find(kq);
        ASSERT_NE(queue, nullptr);
        ASSERT_EQ(queue->registrations.size(), 1u);
        EXPECT_EQ(queue->registrations[0].filter, -10);
        EXPECT_EQ(queue->registrations[0].ident, 1u);
        EXPECT_EQ(queue->registrations[0].udata, 0xDEADu);
    }

    TEST(Kqueue, KeventQosWithAHugeNeventsDoesNotAllocateIt)
    {
        const auto emu = macos_test::make_emulator();

        const auto kq = make_kqueue(*emu, code_base);

        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, // kevent_qos
                    {kq, 0, 0, 0xDEAD0000u, 0x7FFFFFFFu, 0, 0, 0});

        EXPECT_FALSE(failed(*emu)) << "an unmapped eventlist is only faulted when an event is actually placed";
        EXPECT_EQ(result_of(*emu), 0u)
            << "delivery is capped by the pending count, so INT32_MAX nevents allocates nothing and places nothing";
    }

    TEST(Kqueue, EvDeleteRemovesARegistration)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x7;
        registration.filter = -1; // EVFILT_READ
        registration.flags = sogen::MACOS_EV_ADD;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        const auto* queue = emu->process.kqueues.find(kq);
        ASSERT_NE(queue, nullptr);
        ASSERT_EQ(queue->registrations.size(), 1u);

        registration.flags = sogen::MACOS_EV_DELETE;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + 2 * sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_FALSE(failed(*emu));
        EXPECT_TRUE(emu->process.kqueues.find(kq)->registrations.empty());
    }

    TEST(Kqueue, KeventQosOnSomethingThatIsNotAKqueueFails)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, code_base, 374, {0, 0, 0, 0, 0, 0, 0, 0}); // fd 0 is stdin

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF));
    }

    // Measured 2026-08-27 (spec "Measured 2026-08-27"): libdispatch registers on the process
    // workqueue with KEVENT_FLAG_WORKQ in the call flags and the kq argument ignored (0xffffffff).
    // The call itself is the worker-thread request; no changelist entry carries a dedicated bit.
    // Stage A task 4 answers the request at registration time: it is consumed and a worker thread is
    // spawned, so nothing is left pending for a later taker.
    TEST(Kqueue, WorkqCallFlagRecordsAWorkerRequestOnTheProcessWorkqueue)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        sogen::kevent_registration registration{};
        registration.ident = 0x1;
        registration.filter = -10; // EVFILT_USER
        registration.flags = 0x21; // EV_ADD | EV_CLEAR
        registration.udata = 0xFFFFFFFFFFFFFFF8u;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base, 374,                           // kevent_qos
                    {0xFFFFFFFFu, data_base, 1, 0, 0, 0, 0, 0x21}); // IMMEDIATE | WORKQ

        EXPECT_FALSE(failed(*emu)) << "the kq argument is ignored on a workq call, so 0xffffffff is not EBADF";
        EXPECT_EQ(result_of(*emu), 0u);

        const auto* queue = emu->process.kqueues.find(sogen::MACOS_PROCESS_WORKQ_ID);
        ASSERT_NE(queue, nullptr) << "the registration belongs to the process workqueue, not to a descriptor";
        ASSERT_EQ(queue->registrations.size(), 1u);

        EXPECT_FALSE(emu->process.kqueues.has_workq_request(sogen::MACOS_PROCESS_WORKQ_ID))
            << "the request is consumed by the spawn, once per registration";
        EXPECT_EQ(emu->process.threads.size(), 1u) << "and answered with a worker thread";
    }

    // Measured 2026-08-27: kevent_id's first argument is the workloop's dynamic kq id, not a
    // descriptor; the thread request is the changelist entry filter=-17 (EVFILT_WORKLOOP) with
    // fflags NOTE_WL_THREAD_REQUEST. Since task 4 the request is consumed by the spawn.
    TEST(Kqueue, WorkloopThreadRequestEntryRecordsAWorkerRequest)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr uint64_t workloop_id = 0x300518520;

        sogen::kevent_registration registration{};
        registration.ident = workloop_id;
        registration.filter = sogen::MACOS_EVFILT_WORKLOOP;
        registration.flags = 0x5; // EV_ADD | EV_ENABLE
        registration.fflags = 0x111;
        registration.udata = workloop_id;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base, 375,                                            // kevent_id
                    {workloop_id, data_base, 1, data_base + 0x800, 1, 0, 0, 0x403}); // IMMEDIATE | ERROR_EVENTS | WORKLOOP

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);

        EXPECT_FALSE(emu->process.kqueues.take_workq_request(sogen::MACOS_PROCESS_WORKQ_ID).has_value())
            << "the request no longer outlives the call: the worker is spawned at registration time";
        EXPECT_EQ(emu->process.threads.size(), 1u);
    }

    TEST(Kqueue, KeventIdWithoutTheWorkloopFlagIsRefused)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, code_base, 375, {0x1234, 0, 0, 0, 0, 0, 0, 0});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EBADF))
            << "sogen models no dynamic kqueue ids besides workloops";
    }

    TEST(Kqueue, AnUnknownFilterIsAcceptedAndRegistered)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x9;
        registration.filter = -42;
        registration.flags = sogen::MACOS_EV_ADD;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_FALSE(failed(*emu)) << "unmodelled filters are accepted and reported, not refused";
        const auto* queue = emu->process.kqueues.find(kq);
        ASSERT_NE(queue, nullptr);
        ASSERT_EQ(queue->registrations.size(), 1u);
        EXPECT_EQ(queue->registrations[0].filter, -42);
    }

    // Stage A task 6: EVFILT_MACHPORT is the workloop reply demux. Measured 2026-08-27 on the host
    // (cgsdemo under lldb): when a message arrives on a knote-monitored port the delivered event is the
    // registration with EV_VANISHED cleared (0x385 -> 0x185), fflags zeroed, data set to the port name
    // and udata echoed.
    TEST(Kqueue, AMachportKnoteFiresWhenAMessageArrivesOnThePort)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto port = emu->mach.ports.allocate_receive_right();

        sogen::kevent_registration registration{};
        registration.ident = port;
        registration.filter = sogen::MACOS_EVFILT_MACHPORT;
        registration.flags = 0x385;
        registration.fflags = 0x0700080e;
        registration.udata = 0xDEAD;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 0u) << "an empty port fires nothing";

        emu->mach.ports.find(port)->queue.emplace_back(24, 0);
        EXPECT_EQ(emu->process.kqueues.note_port_message(port), 1u);

        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(events[0].ident, uint64_t{port});
        EXPECT_EQ(events[0].flags, 0x185u);
        EXPECT_EQ(events[0].fflags, 0u);
        EXPECT_EQ(events[0].data, static_cast<int64_t>(port));
        EXPECT_EQ(events[0].udata, 0xDEADu);
    }

    // The same knote is level-triggered: a message already queued when the knote is registered fires
    // the event at registration time, the way the kernel reports a readable port immediately.
    TEST(Kqueue, AMachportKnoteOnANonEmptyPortFiresAtRegistration)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto port = emu->mach.ports.allocate_receive_right();
        emu->mach.ports.find(port)->queue.emplace_back(24, 0);

        sogen::kevent_registration registration{};
        registration.ident = port;
        registration.filter = sogen::MACOS_EVFILT_MACHPORT;
        registration.flags = 0x385;
        registration.fflags = 0x0700080e;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 1u);
    }

    // Measured 2026-08-27 on the host: libdispatch registers a connection's EVFILT_MACHPORT knote
    // through kevent_id on the workloop's dynamic kq id, so the registration has to survive there.
    TEST(Kqueue, KeventIdStoresMachportRegistrationsOnTheWorkloopId)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr uint64_t workloop_id = 0x300518520;
        const auto port = emu->mach.ports.allocate_receive_right();

        sogen::kevent_registration registration{};
        registration.ident = port;
        registration.filter = sogen::MACOS_EVFILT_MACHPORT;
        registration.flags = 0x385;
        registration.fflags = 0x0700080e;
        registration.udata = 0xBEEF;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base, 375,                                            // kevent_id
                    {workloop_id, data_base, 1, data_base + 0x800, 1, 0, 0, 0x403}); // IMMEDIATE | ERROR_EVENTS | WORKLOOP

        EXPECT_FALSE(failed(*emu));
        const auto* queue = emu->process.kqueues.find(workloop_id);
        ASSERT_NE(queue, nullptr) << "the workloop id keys a real kqueue";
        ASSERT_EQ(queue->registrations.size(), 1u);
        EXPECT_EQ(queue->registrations[0].filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(queue->registrations[0].ident, uint64_t{port});
        EXPECT_EQ(emu->process.threads.size(), 0u) << "a machport registration alone requests no worker";

        emu->mach.ports.find(port)->queue.emplace_back(24, 0);
        EXPECT_EQ(emu->process.kqueues.note_port_message(port), 1u);
        EXPECT_EQ(emu->process.kqueues.pending_count(workloop_id), 1u);
    }

    // Measured 2026-08-28 on the host: the delivered event is the registration with EV_CLEAR added,
    // the fflags zeroed and data set to how many expirations the guest has not collected. udata, qos
    // and ext come back untouched.
    TEST(KqueueTimer, ADispatchShapedTimerFiresWhenItsDeadlineArrives)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto deadline = counter_of(*emu) + nanoseconds_in_ticks(*emu, 1500000000);

        write_entry(*emu, data_base, dispatch_timer(deadline));
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        EXPECT_FALSE(failed(*emu));

        EXPECT_EQ(emu->process.kqueues.earliest_timer_deadline(), deadline);
        EXPECT_EQ(emu->process.kqueues.fire_due_timers(deadline - 1), 0u) << "a deadline that has not arrived is not due";
        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 0u);

        EXPECT_EQ(emu->process.kqueues.fire_due_timers(deadline), 1u);

        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].filter, sogen::MACOS_EVFILT_TIMER);
        EXPECT_EQ(events[0].ident, 0xFFFFFFFFFFFFFF00ULL);
        EXPECT_EQ(events[0].flags, 0x35u) << "EV_ADD | EV_ENABLE | EV_ONESHOT | EV_CLEAR";
        EXPECT_EQ(events[0].fflags, 0u) << "the NOTE_* bits do not come back";
        EXPECT_EQ(events[0].data, 1);
        EXPECT_EQ(events[0].udata, 0x1F9A5BD70ULL);
        EXPECT_EQ(events[0].ext[1], 0x1B773Fu) << "the leeway is echoed";
    }

    // filt_timerattach forces EV_ONESHOT onto every NOTE_ABSOLUTE knote (measured: a knote added with
    // 0x5 comes back as 0x35 and never fires twice), and a delivered one-shot is gone.
    TEST(KqueueTimer, AnAbsoluteTimerIsAOneShotEvenWithoutEvOneshot)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto deadline = counter_of(*emu) + nanoseconds_in_ticks(*emu, 120000000);

        auto registration = dispatch_timer(deadline);
        registration.flags = 0x5; // EV_ADD | EV_ENABLE
        registration.fflags = sogen::MACOS_NOTE_ABSOLUTE | sogen::MACOS_NOTE_MACHTIME;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        ASSERT_EQ(emu->process.kqueues.fire_due_timers(deadline), 1u);

        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].flags, 0x35u);

        EXPECT_TRUE(emu->process.kqueues.find(kq)->registrations.empty()) << "a delivered one-shot is deleted";
        EXPECT_FALSE(emu->process.kqueues.earliest_timer_deadline().has_value());
        EXPECT_EQ(emu->process.kqueues.fire_due_timers(deadline + 1000000), 0u);
    }

    // Measured 2026-08-28: an absolute deadline the guest names in the past is due at once -- the very
    // next kevent returned it with no wait.
    TEST(KqueueTimer, AnAbsoluteDeadlineInThePastFiresAtRegistration)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        write_entry(*emu, data_base, dispatch_timer(1));
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 1u) << "the changelist itself produced the event";
    }

    // Measured 2026-08-28: a 100 ms interval knote left unread for 350 ms delivered one event with
    // data=3, and the knote stayed registered on its original phase.
    TEST(KqueueTimer, AnIntervalTimerRepeatsAndCountsWhatWentUncollected)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto interval = nanoseconds_in_ticks(*emu, 100000000);

        sogen::kevent_registration registration{};
        registration.ident = 0x5;
        registration.filter = sogen::MACOS_EVFILT_TIMER;
        registration.flags = 0x5; // EV_ADD | EV_ENABLE
        registration.fflags = sogen::MACOS_NOTE_NSECONDS;
        registration.data = 100000000;
        write_entry(*emu, data_base, registration);

        const auto before = counter_of(*emu);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        const auto after = counter_of(*emu);

        const auto* queue = emu->process.kqueues.find(kq);
        ASSERT_NE(queue, nullptr);
        ASSERT_EQ(queue->timers.size(), 1u);
        const auto armed = queue->timers.at(0x5);
        EXPECT_TRUE(armed.repeating);
        EXPECT_EQ(armed.interval, interval);
        EXPECT_GE(armed.deadline, before + interval);
        EXPECT_LE(armed.deadline, after + interval);

        ASSERT_EQ(emu->process.kqueues.fire_due_timers(armed.deadline + 2 * interval + interval / 2), 1u);

        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].flags, 0x25u) << "EV_ADD | EV_ENABLE | EV_CLEAR, and no EV_ONESHOT";
        EXPECT_EQ(events[0].data, 3);

        ASSERT_EQ(queue->registrations.size(), 1u) << "an interval knote survives its delivery";
        EXPECT_EQ(queue->timers.at(0x5).deadline, armed.deadline + 3 * interval) << "and stays on its original phase";
        EXPECT_EQ(queue->timers.at(0x5).fired, 0u) << "delivering the event resets the uncollected count";
    }

    // Measured 2026-08-28: data with no unit bit at all is milliseconds -- data=150 fired after 151.7 ms.
    TEST(KqueueTimer, DataWithNoUnitNoteIsMilliseconds)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x2;
        registration.filter = sogen::MACOS_EVFILT_TIMER;
        registration.flags = 0x15;
        registration.data = 150;
        write_entry(*emu, data_base, registration);

        const auto before = counter_of(*emu);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        const auto after = counter_of(*emu);

        const auto milliseconds = nanoseconds_in_ticks(*emu, 150 * 1000000);
        const auto armed = emu->process.kqueues.find(kq)->timers.at(0x2);
        EXPECT_GE(armed.deadline, before + milliseconds);
        EXPECT_LE(armed.deadline, after + milliseconds);
    }

    // filt_timervalidate refuses a registration that names two units. sogen has no EV_ERROR channel to
    // report it per entry, so the call is refused as a whole.
    TEST(KqueueTimer, NamingTwoUnitsAtOnceIsRefused)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x3;
        registration.filter = sogen::MACOS_EVFILT_TIMER;
        registration.flags = 0x15;
        registration.fflags = sogen::MACOS_NOTE_SECONDS | sogen::MACOS_NOTE_NSECONDS;
        registration.data = 1;
        write_entry(*emu, data_base, registration);

        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), static_cast<uint64_t>(sogen::macos_errno::MACOS_EINVAL));
        EXPECT_TRUE(emu->process.kqueues.find(kq)->registrations.empty());
    }

    TEST(KqueueTimer, EvDeleteDisarmsTheTimerAsWellAsTheKnote)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);
        const auto deadline = counter_of(*emu) + nanoseconds_in_ticks(*emu, 500000000);

        write_entry(*emu, data_base, dispatch_timer(deadline));
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});
        ASSERT_TRUE(emu->process.kqueues.earliest_timer_deadline().has_value());

        auto removal = dispatch_timer(deadline);
        removal.flags = sogen::MACOS_EV_DELETE;
        write_entry(*emu, data_base, removal);
        run_syscall(*emu, code_base + 2 * sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_FALSE(emu->process.kqueues.earliest_timer_deadline().has_value());
        EXPECT_TRUE(emu->process.kqueues.find(kq)->timers.empty());
        EXPECT_EQ(emu->process.kqueues.fire_due_timers(deadline + 1), 0u);
    }

    // Measured 2026-08-28: EV_DISABLE stops delivery entirely, and the EV_ENABLE that follows fires at
    // once with data=1 rather than with the count of the intervals it slept through -- the re-enable
    // re-phases the deadline instead of catching up. The interval here is one tick, so a knote that
    // caught up would report the thousands of ticks the two calls took.
    TEST(KqueueTimer, DisableStopsATimerAndEnableRePhasesIt)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto kq = make_kqueue(*emu, code_base);

        sogen::kevent_registration registration{};
        registration.ident = 0x8;
        registration.filter = sogen::MACOS_EVFILT_TIMER;
        registration.flags = 0x5; // EV_ADD | EV_ENABLE
        registration.fflags = sogen::MACOS_NOTE_NSECONDS;
        registration.data = static_cast<int64_t>(emu->mach.timebase_numer);
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        const auto armed = emu->process.kqueues.find(kq)->timers.at(0x8).deadline;
        ASSERT_EQ(emu->process.kqueues.pending_count(kq), 0u);

        registration.flags = sogen::MACOS_EV_DISABLE;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + 2 * sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        EXPECT_FALSE(emu->process.kqueues.earliest_timer_deadline().has_value()) << "a disabled timer owes the scheduler nothing";
        EXPECT_EQ(emu->process.kqueues.fire_due_timers(armed + 1000), 0u);
        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 0u);

        registration.flags = sogen::MACOS_EV_ENABLE;
        write_entry(*emu, data_base, registration);
        run_syscall(*emu, code_base + 3 * sogen::MACOS_PAGE_SIZE, 374, {kq, data_base, 1, 0, 0, 0, 0, 0});

        ASSERT_EQ(emu->process.kqueues.pending_count(kq), 1u) << "the deadline is long past, so the re-enable is itself the expiry";
        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].data, 1) << "the wait it slept through is not caught up on";
    }

    // Measured 2026-08-28: libdispatch's own KEVENT_FLAG_WORKQ call passes a 16-entry eventlist and is
    // answered with 0 -- a workq event belongs to the workqueue thread the kernel hands it to, never to
    // whoever happens to be calling.
    TEST(KqueueTimer, AWorkqTimerEventIsNotHandedToTheKeventCaller)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        write_entry(*emu, data_base, dispatch_timer(1));
        run_syscall(*emu, code_base, 374, // kevent_qos, IMMEDIATE | WORKQ
                    {0xFFFFFFFFu, data_base, 1, data_base + 0x800, 16, 0, 0, 0x21});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u) << "the caller is told about none of them";
        EXPECT_EQ(emu->process.kqueues.pending_count(sogen::MACOS_PROCESS_WORKQ_ID), 1u) << "and the event is left for a worker";
    }

    // Measured 2026-08-28 on the host under lldb: every libdispatch timer is registered by a workqueue
    // worker inside the __workq_kernreturn that parks it -- WQOPS_THREAD_KEVENT_RETURN with the
    // changelist in x1 -- and never through kevent_qos, so the return path has to apply it.
    TEST(KqueueTimer, AWorkerReturnChangelistArmsTheTimer)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto deadline = counter_of(*emu) + nanoseconds_in_ticks(*emu, 1000000000);
        write_entry(*emu, data_base, dispatch_timer(deadline));

        const sogen::macos_syscall_context context{*emu, emu->emu(), emu->process};
        sogen::apply_worker_return_changelist(context, data_base, 1, false);

        EXPECT_EQ(emu->process.kqueues.earliest_timer_deadline(), deadline);
        const auto* queue = emu->process.kqueues.find(sogen::MACOS_PROCESS_WORKQ_ID);
        ASSERT_NE(queue, nullptr) << "a plain kevent return registers on the process workqueue";
        ASSERT_EQ(queue->registrations.size(), 1u);
        EXPECT_EQ(queue->registrations[0].filter, sogen::MACOS_EVFILT_TIMER);
    }

    // Measured 2026-08-28: NOTE_ABSOLUTE without NOTE_MACHTIME counts from the calendar clock, not from
    // mach absolute time -- a deadline built from mach_absolute_time fired instantly, the same deadline
    // built from gettimeofday fired after the interval it named.
    TEST(KqueueTimer, AnAbsoluteDeadlineWithAUnitIsMeasuredFromTheCalendarClock)
    {
        const sogen::kevent_timer_clock clock{
            .now = 1000, .timebase_numer = 16, .timebase_denom = 1, .calendar_ns = 1'750'000'000'000'000'000ULL};

        sogen::kevent_registration change{};
        change.filter = sogen::MACOS_EVFILT_TIMER;
        change.flags = 0x15;
        change.fflags = sogen::MACOS_NOTE_ABSOLUTE | sogen::MACOS_NOTE_NSECONDS;
        change.data = static_cast<int64_t>(clock.calendar_ns + 140'000'000ULL);

        const auto resolved = sogen::resolve_kevent_timer(change, clock);
        ASSERT_TRUE(resolved.has_value());
        EXPECT_FALSE(resolved->repeating);
        EXPECT_EQ(resolved->deadline, clock.now + 140'000'000ULL / 16);

        change.fflags = sogen::MACOS_NOTE_ABSOLUTE | sogen::MACOS_NOTE_SECONDS;
        change.data = 1;
        const auto past = sogen::resolve_kevent_timer(change, clock);
        ASSERT_TRUE(past.has_value());
        EXPECT_EQ(past->deadline, clock.now) << "a calendar deadline already gone is due now";
    }
}
