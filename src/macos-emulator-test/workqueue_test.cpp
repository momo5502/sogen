#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t changelist = 0x300000000ULL;

    void run_syscall_at(sogen::macos_emulator& emu, const uint64_t address, const uint32_t mov_x16)
    {
        if (!emu.memory.get_region_info(address).has_value())
        {
            emu.memory.allocate_memory(address, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);
        }

        const std::array<uint32_t, 2> words{mov_x16, 0xD4001001};
        emu.memory.write_memory(address, words.data(), sizeof(words));
        emu.emu().reg(sogen::arm64_register::pc, address);
        emu.start(2);
    }

    // The measured first registration of the Stage A spec ("Measured 2026-08-27"): a kevent_qos call
    // with KEVENT_FLAG_WORKQ is libdispatch asking the kernel for a worker thread, and the answer is a
    // real guest thread started at _start_wqthread with the register contract __pthread_wqthread reads.
    TEST(Workqueue, AWorkqRegistrationSpawnsAWorkerAtStartWqthread)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        sogen::macos_kevent_qos_entry entry{};
        entry.ident = 0x1;
        entry.filter = -10; // EVFILT_USER
        entry.flags = 0x21; // EV_ADD | EV_CLEAR
        entry.qos = 0x2000000;
        entry.udata = 0xFFFFFFFFFFFFFFF8ULL;
        emu->memory.write_memory(changelist, &entry, sizeof(entry));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0xFFFFFFFF}); // kq, ignored under WORKQ
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{1});          // nchanges
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});          // eventlist
        emu->emu().reg(sogen::arm64_register::x4, uint64_t{0});          // nevents
        emu->emu().reg(sogen::arm64_register::x7, uint64_t{0x21});       // IMMEDIATE | WORKQ
        run_syscall_at(*emu, code_base, macos_test::movz_x(16, 374, 0)); // kevent_qos

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        ASSERT_EQ(emu->process.threads.size(), 1u) << "the registration is a thread request";

        const auto& worker = emu->process.threads.begin()->second;

        constexpr uint64_t stack_base = sogen::MACOS_WORKQUEUE_ARENA_BASE;
        constexpr uint64_t pthread_page = stack_base + sogen::MACOS_WORKQUEUE_STACK_SIZE;

        // Without a shared cache the measured export stands in, and the trace says so; the value itself
        // is pinned by the spec's measurement.
        EXPECT_EQ(sogen::MACOS_START_WQTHREAD_FALLBACK, 0x1804FAC08ull);
        EXPECT_EQ(worker.saved_regs.pc, sogen::MACOS_START_WQTHREAD_FALLBACK);
        EXPECT_EQ(worker.saved_regs.sp, stack_base + sogen::MACOS_WORKQUEUE_STACK_SIZE);
        EXPECT_EQ(worker.saved_regs.x[0], pthread_page) << "x0 = self";
        EXPECT_NE(worker.saved_regs.x[1], 0u) << "x1 = kport, a real mach port name";
        EXPECT_EQ(worker.saved_regs.x[2], stack_base) << "x2 = stacklowaddr";
        EXPECT_EQ(worker.saved_regs.x[3], 0u) << "x3 = keventlist: measured NULL for a live host worker";
        EXPECT_EQ(worker.saved_regs.x[4], uint64_t{sogen::MACOS_WQTHREAD_SPAWN_FLAGS});
        EXPECT_EQ(sogen::MACOS_WQTHREAD_SPAWN_FLAGS, 0x244005u)
            << "measured at __pthread_wqthread's entry in a live process; x4=0 traps in _pthread_wqthread_setup";
        EXPECT_EQ(worker.saved_regs.x[5], 0u) << "x5 = nkevents: measured 0 for a live host worker";
        EXPECT_EQ(worker.saved_regs.tpidrro_el0, pthread_page + sogen::MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET);

        // _pthread_wqthread_setup stores x1 where pthread_mach_thread_np reads it, so it has to be the
        // same name mach_thread_self() answers for this thread. A pool-private port would give the
        // worker two identities and trip libplatform's ownership assertion on its first os_unfair_lock.
        const auto object = emu->mach.ports.object_of(static_cast<sogen::mach::port_name_t>(worker.saved_regs.x[1]));
        EXPECT_EQ(object.kind, sogen::mach::kernel_object_kind::thread);
        EXPECT_EQ(object.id, worker.thread_id);
        EXPECT_EQ(worker.saved_regs.x[1], emu->mach.thread_self_for(worker.thread_id));
    }

    // The second measured trigger: a workloop registration carrying NOTE_WL_THREAD_REQUEST, pinned in
    // the same measurement as the only entry the xnu headers literally name a thread request.
    //
    // Measured 2026-08-27 on the host (cgsdemo under lldb, breakpoint on start_wqthread): a workloop
    // spawn differs from the plain workq spawn -- flags carry the workloop bit, and the kernel places
    // the workloop's own registration event into a kevent buffer 0x480 below the pthread page and hands
    // it over in x3/x5.
    TEST(Workqueue, AWorkloopThreadRequestSpawnsAWorkerToo)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t workloop_id = 0x300518520;

        sogen::macos_kevent_qos_entry entry{};
        entry.ident = workloop_id;
        entry.filter = sogen::MACOS_EVFILT_WORKLOOP;
        entry.flags = 0x5; // EV_ADD | EV_ENABLE
        entry.qos = 0x8FF;
        entry.udata = workloop_id;
        entry.fflags = 0x111; // NOTE_WL_THREAD_REQUEST | NOTE_WL_UPDATE_QOS | NOTE_WL_IGNORE_ESTALE
        emu->memory.write_memory(changelist, &entry, sizeof(entry));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{workloop_id});
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{1});
        emu->emu().reg(sogen::arm64_register::x7, uint64_t{0x403});      // IMMEDIATE | ERROR_EVENTS | WORKLOOP
        run_syscall_at(*emu, code_base, macos_test::movz_x(16, 375, 0)); // kevent_id

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        ASSERT_EQ(emu->process.threads.size(), 1u);

        const auto& worker = emu->process.threads.begin()->second;

        constexpr uint64_t stack_base = sogen::MACOS_WORKQUEUE_ARENA_BASE;
        constexpr uint64_t pthread_page = stack_base + sogen::MACOS_WORKQUEUE_STACK_SIZE;
        constexpr uint64_t event_buffer = pthread_page - 0x480;

        EXPECT_EQ(worker.saved_regs.pc, sogen::MACOS_START_WQTHREAD_FALLBACK);
        EXPECT_EQ(worker.saved_regs.x[3], event_buffer);
        EXPECT_EQ(worker.saved_regs.x[5], 1u);
        EXPECT_EQ(worker.saved_regs.x[4], uint64_t{sogen::MACOS_WQTHREAD_WORKLOOP_SPAWN_FLAGS});
        EXPECT_EQ(sogen::MACOS_WQTHREAD_WORKLOOP_SPAWN_FLAGS, 0x6C4004u)
            << "measured at start_wqthread for a workloop spawn: the workloop bit (0x400000) is set and REUSE (0x20000) is not";

        sogen::macos_kevent_qos_entry delivered{};
        emu->memory.read_memory(event_buffer, &delivered, sizeof(delivered));
        EXPECT_EQ(delivered.ident, workloop_id);
        EXPECT_EQ(delivered.filter, sogen::MACOS_EVFILT_WORKLOOP);
        EXPECT_EQ(delivered.flags, 0x25u) << "measured: the delivered thread-request event gains EV_CLEAR";
        EXPECT_EQ(delivered.fflags, 0x111u);
        EXPECT_EQ(delivered.udata, workloop_id);
        EXPECT_EQ(delivered.qos, 0x8FF) << "the registration's qos is echoed into the delivered event";

        uint64_t id_word = 0;
        emu->memory.read_memory(event_buffer - 8, &id_word, sizeof(id_word));
        EXPECT_EQ(id_word, workloop_id) << "measured: the workloop kq id sits in the word below the event list";
    }

    // The measured wake contract (same host measurement): a pool-parked worker is re-entered at
    // start_wqthread with REUSE set, the pending events written into its kevent buffer, x5 = count.
    TEST(Workqueue, WakingAPoolParkedWorkerDeliversTheEventAtStartWqthread)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t worker_stack = 0x321000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(worker_stack, 9 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const auto kport = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::workqueue, .id = 1});
        const auto worker = emu->process.create_thread(worker_stack, 8 * sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.threads.at(worker).workqueue_kport = kport;
        emu->process.threads.at(worker).blocked_on_port = kport; // parked in the pool

        const auto port = emu->mach.ports.allocate_receive_right();
        emu->mach.ports.find(port)->queue.emplace_back(24, 0);

        auto& workq = emu->process.kqueues.ensure(sogen::MACOS_PROCESS_WORKQ_ID);
        workq.registrations.push_back({.filter = sogen::MACOS_EVFILT_MACHPORT, .ident = port, .flags = 0x385, .udata = 0x1234});
        ASSERT_EQ(emu->process.kqueues.note_port_message(port), 1u);

        ASSERT_TRUE(emu->workqueue.wake_parked_worker(*emu));

        const auto& woken = emu->process.threads.at(worker);
        EXPECT_EQ(woken.blocked_on_port, 0u);
        EXPECT_EQ(woken.saved_regs.pc, sogen::MACOS_START_WQTHREAD_FALLBACK);
        EXPECT_EQ(woken.saved_regs.sp, worker_stack + 8 * sogen::MACOS_PAGE_SIZE);
        EXPECT_EQ(woken.saved_regs.x[0], worker_stack + 8 * sogen::MACOS_PAGE_SIZE) << "x0 = self, the pthread page";
        EXPECT_EQ(woken.saved_regs.x[1], uint64_t{emu->mach.thread_self_for(worker)}) << "a reused worker keeps its own thread port";
        EXPECT_EQ(woken.saved_regs.x[2], worker_stack);
        EXPECT_EQ(woken.saved_regs.x[3], worker_stack + 8 * sogen::MACOS_PAGE_SIZE - 0x480);
        EXPECT_EQ(woken.saved_regs.x[4], uint64_t{sogen::MACOS_WQTHREAD_WORKQ_WAKE_FLAGS});
        EXPECT_EQ(woken.saved_regs.x[5], 1u);

        sogen::macos_kevent_qos_entry delivered{};
        emu->memory.read_memory(woken.saved_regs.x[3], &delivered, sizeof(delivered));
        EXPECT_EQ(delivered.ident, uint64_t{port});
        EXPECT_EQ(delivered.filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(delivered.udata, 0x1234u);
    }

    // A worker that parks while workloop events are pending is continued with them instead of sleeping
    // through its own wake.
    TEST(Workqueue, AParkWithPendingEventsContinuesTheWorker)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t worker_stack = 0x321000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(worker_stack, 9 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const std::array<uint32_t, 2> park_words{macos_test::movz_x(16, 368, 0), 0xD4001001}; // workq_kernreturn
        emu->memory.write_memory(code_base, park_words.data(), sizeof(park_words));

        const auto kport = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::workqueue, .id = 1});
        const auto worker = emu->process.create_thread(worker_stack, 8 * sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.threads.at(worker).workqueue_kport = kport;
        ASSERT_TRUE(emu->activate_thread(worker));

        constexpr uint64_t workloop_id = 0x300518520;
        auto& workloop = emu->process.kqueues.ensure(workloop_id);
        workloop.is_workloop = true;
        workloop.pending.push_back({.filter = sogen::MACOS_EVFILT_MACHPORT, .ident = 0x777, .flags = 0x185});

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_WQOPS_THREAD_RETURN});
        emu->emu().reg(sogen::arm64_register::pc, code_base);

        // The continuation re-enters the thread at start_wqthread; without a shared cache that is the
        // measured fallback address, mapped here with a brk so the run stops as the worker lands there.
        const uint64_t entry_page = sogen::MACOS_START_WQTHREAD_FALLBACK & ~0xFFFULL;
        ASSERT_TRUE(emu->memory.allocate_memory(entry_page, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const uint32_t brk = 0xD4200000;
        emu->memory.write_memory(sogen::MACOS_START_WQTHREAD_FALLBACK, &brk, sizeof(brk));

        emu->start();

        const auto& continued = emu->process.threads.at(worker);
        EXPECT_EQ(continued.blocked_on_port, 0u) << "the worker did not park: events were waiting";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), sogen::MACOS_START_WQTHREAD_FALLBACK);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x5), 1u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x4), uint64_t{sogen::MACOS_WQTHREAD_WORKLOOP_WAKE_FLAGS});
        EXPECT_TRUE(emu->process.kqueues.find(workloop_id)->pending.empty());
    }

    // WQOPS_THREAD_RETURN (xnu bsd/pthread/workqueue_syscalls.h) parks the worker back into the kernel
    // pool: the call does not return to userspace on a real kernel, so the worker waits on its own
    // kport and the cpu goes to whoever can still run.
    TEST(Workqueue, ThreadReturnParksTheWorkerOnItsKport)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t other_code = code_base + 0x800;
        constexpr uint64_t worker_stack = 0x321000000ULL;
        constexpr uint64_t other_stack = 0x322000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(worker_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(other_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const std::array<uint32_t, 2> park_words{macos_test::movz_x(16, 368, 0), 0xD4001001}; // workq_kernreturn
        emu->memory.write_memory(code_base, park_words.data(), sizeof(park_words));

        const std::array<uint32_t, 4> exit_words{
            macos_test::movz_x(0, 41, 0),
            macos_test::movz_x(16, 1, 0),
            0xD4001001,
            0xD4200000,
        };
        emu->memory.write_memory(other_code, exit_words.data(), sizeof(exit_words));

        const auto kport = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::workqueue, .id = 1});

        const auto worker = emu->process.create_thread(worker_stack, sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.threads.at(worker).workqueue_kport = kport;
        const auto other = emu->process.create_thread(other_stack, sogen::MACOS_PAGE_SIZE, other_code);
        ASSERT_TRUE(emu->activate_thread(worker));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_WQOPS_THREAD_RETURN});
        emu->emu().reg(sogen::arm64_register::pc, code_base);

        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41);
        EXPECT_EQ(emu->process.active_thread->thread_id, other) << "the parked worker gave the cpu up";
        EXPECT_EQ(emu->process.threads.at(worker).blocked_on_port, kport) << "the worker waits on its own kport";
    }

    // With nothing else runnable, a pool park is a deadlock like any other wait and is named as one.
    TEST(Workqueue, ThreadReturnWithNobodyLeftToRunIsANamedDeadlock)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t worker_stack = 0x323000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(worker_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));

        const std::array<uint32_t, 2> words{macos_test::movz_x(16, 368, 0), 0xD4001001};
        emu->memory.write_memory(code_base, words.data(), sizeof(words));

        const auto kport = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::workqueue, .id = 1});

        const auto worker = emu->process.create_thread(worker_stack, sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.threads.at(worker).workqueue_kport = kport;
        ASSERT_TRUE(emu->activate_thread(worker));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_WQOPS_THREAD_RETURN});
        emu->emu().reg(sogen::arm64_register::pc, code_base);

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::workqueue_park_deadlock);
        EXPECT_EQ(emu->process.threads.at(worker).blocked_on_port, 0u) << "a thread that could not be parked is not left marked";
    }

    // UL_COMPARE_AND_WAIT compares first: the word changing before the wait means there is nothing to
    // wait for, and parking there would sleep through the wake that already happened.
    TEST(ProcessSyscalls, UlockWaitReturnsImmediatelyWhenTheValueNoLongerMatches)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const uint32_t locked = 0x11111111;
        emu->memory.write_memory(changelist, &locked, sizeof(locked));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_UL_COMPARE_AND_WAIT});
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{0x22222222});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
        run_syscall_at(*emu, code_base, macos_test::movz_x(16, 515, 0)); // ulock_wait

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u);
    }

    // The futex half of the workqueue loop: a matching value parks the thread and the emulator switches
    // away, exactly like the mach receive park it is modelled on.
    TEST(ProcessSyscalls, UlockWaitParksTheThreadAndUlockWakeClearsOneWaiter)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t other_code = code_base + 0x800;
        constexpr uint64_t waiter_stack = 0x321000000ULL;
        constexpr uint64_t other_stack = 0x322000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(waiter_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(other_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const uint32_t value = 0xABCD;
        emu->memory.write_memory(changelist, &value, sizeof(value));

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const std::array<uint32_t, 2> wait_words{macos_test::movz_x(16, 515, 0), 0xD4001001};
        emu->memory.write_memory(code_base, wait_words.data(), sizeof(wait_words));

        // Wakes the address, then exits: the exit code is what separates "the wake ran" from any other
        // way the run could have stopped.
        const std::array<uint32_t, 6> wake_words{
            macos_test::movz_x(16, 516, 0),
            0xD4001001, // ulock_wake
            macos_test::movz_x(0, 41, 0),
            macos_test::movz_x(16, 1, 0),
            0xD4001001,
            0xD4200000,
        };
        emu->memory.write_memory(other_code, wake_words.data(), sizeof(wake_words));

        const auto waiter = emu->process.create_thread(waiter_stack, sogen::MACOS_PAGE_SIZE, code_base);
        const auto waker = emu->process.create_thread(other_stack, sogen::MACOS_PAGE_SIZE, other_code);

        // The waker's own syscall arguments: a thread starts with zeroed saved registers, and the wake
        // needs its operation and address as much as the wait did.
        auto& waker_regs = emu->process.threads.at(waker).saved_regs;
        waker_regs.x[0] = sogen::MACOS_UL_COMPARE_AND_WAIT;
        waker_regs.x[1] = changelist;

        ASSERT_TRUE(emu->activate_thread(waiter));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_UL_COMPARE_AND_WAIT});
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{value});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::pc, code_base);

        emu->start();

        EXPECT_EQ(emu->process.exit_status, 41);
        EXPECT_EQ(emu->process.active_thread->thread_id, waker) << "the parked waiter gave the cpu up";
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_ulock, 0u) << "the wake cleared the one waiter on the address";
    }

    // With nothing else runnable nobody can ever store to the word or wake the address, so the wait is
    // a deadlock and has to be named as one rather than left spinning.
    TEST(ProcessSyscalls, UlockWaitWithNobodyLeftToRunIsANamedDeadlock)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        constexpr uint64_t only_stack = 0x323000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(only_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const uint32_t value = 0xABCD;
        emu->memory.write_memory(changelist, &value, sizeof(value));

        ASSERT_TRUE(emu->memory.allocate_memory(code_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const std::array<uint32_t, 2> words{macos_test::movz_x(16, 515, 0), 0xD4001001};
        emu->memory.write_memory(code_base, words.data(), sizeof(words));

        const auto only = emu->process.create_thread(only_stack, sogen::MACOS_PAGE_SIZE, code_base);
        ASSERT_TRUE(emu->activate_thread(only));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{sogen::MACOS_UL_COMPARE_AND_WAIT});
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        emu->emu().reg(sogen::arm64_register::x2, uint64_t{value});
        emu->emu().reg(sogen::arm64_register::x3, uint64_t{0});
        emu->emu().reg(sogen::arm64_register::pc, code_base);

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::ulock_wait_deadlock);
        EXPECT_EQ(emu->process.threads.at(only).blocked_on_ulock, 0u) << "a thread that could not be parked is not left marked";
        EXPECT_EQ(emu->emu().read_instruction_pointer(), code_base + sizeof(words))
            << "and the rewind was undone, so the halt reports the pc after the trap";
    }

    TEST(ProcessSyscalls, UlockOperationsRefuseWhatIsNotModelled)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(changelist, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x9999});
        emu->emu().reg(sogen::arm64_register::x1, changelist);
        run_syscall_at(*emu, code_base, macos_test::movz_x(16, 515, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u)
            << "an unknown ulock_wait operation fails rather than parking on a guess";

        emu->emu().reg(sogen::arm64_register::x0, uint64_t{0x9999});
        run_syscall_at(*emu, code_base + 0x40, macos_test::movz_x(16, 516, 0));
        EXPECT_NE(emu->emu().reg(sogen::arm64_register::nzcv) & sogen::MACOS_NZCV_CARRY, 0u)
            << "an unknown ulock_wake operation fails rather than waking nothing silently";
    }
}
