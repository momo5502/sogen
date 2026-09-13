#pragma once

#include "std_include.hpp"

#include <arch_emulator.hpp>

namespace sogen
{
    constexpr size_t MACOS_GENERAL_REGISTER_COUNT = 29;
    constexpr size_t MACOS_VECTOR_REGISTER_COUNT = 32;

    using macos_vector_register = std::array<uint64_t, 2>;

    struct macos_saved_registers
    {
        std::array<uint64_t, MACOS_GENERAL_REGISTER_COUNT> x{};
        uint64_t fp{};
        uint64_t lr{};
        uint64_t sp{};
        uint64_t pc{};
        uint64_t nzcv{};
        uint64_t tpidrro_el0{};
        uint64_t tpidr_el0{};

        // AAPCS64 makes the low 64 bits of v8..v15 callee-saved, so a thread parked on the svc inside a
        // syscall stub still owns them, and v0..v7 and v16..v31 are live whenever the switch lands
        // somewhere other than a call boundary. Leaving them out of the context hands the resumed thread
        // whatever the other thread last computed.
        std::array<macos_vector_register, MACOS_VECTOR_REGISTER_COUNT> v{};
        uint32_t fpcr{};
        uint32_t fpsr{};
    };

    static_assert(static_cast<uint32_t>(arm64_register::x28) == static_cast<uint32_t>(arm64_register::x0) + 28,
                  "x0..x28 must be contiguous for index based save and restore");

    static_assert(static_cast<uint32_t>(arm64_register::v31) == static_cast<uint32_t>(arm64_register::v0) + 31,
                  "v0..v31 must be contiguous for index based save and restore");

    namespace macos_thread_detail
    {
        constexpr arm64_register general_register(const size_t index)
        {
            return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::x0) + static_cast<uint32_t>(index));
        }

        constexpr arm64_register vector_register(const size_t index)
        {
            return static_cast<arm64_register>(static_cast<uint32_t>(arm64_register::v0) + static_cast<uint32_t>(index));
        }
    }

    struct macos_thread
    {
        uint64_t thread_id{};
        uint64_t stack_base{};
        uint64_t stack_size{};
        uint64_t thread_self{};
        bool terminated{};

        // Non-zero while the thread is parked in a receive on this port. A parked thread's saved pc points
        // at its own svc, so scheduling it again re-runs the receive: that is how a real kernel restarts an
        // interrupted syscall, and it is what lets the emulator switch away from a wait instead of
        // halting on it.
        uint32_t blocked_on_port{};

        // The last message this thread sent. A thread parked in a receive is nearly always waiting for
        // the answer to one particular request, and the port name it waits on never says which -- so
        // without this a deadlock report names the wait but not the question.
        // bsdthread_ctl SET_SELF's pthread_priority word. Recorded, not scheduled on.
        uint32_t pthread_priority{};

        uint32_t last_send_port{};
        int32_t last_send_routine{};

        // The ulock_wait counterpart of the park above: non-zero while the thread waits on this futex
        // word. An address rather than a port name, so the two never share a field -- a port name could
        // alias a low guest address and wake the wrong wait.
        uint64_t blocked_on_ulock{};

        // The __semwait_signal park: the condition semaphore's port name while the thread waits on it,
        // and the wait's deadline in mach-absolute-time ticks, zero for an indefinite wait. The svc
        // re-runs when the thread is scheduled again, and the two markers tell the re-run what ended
        // the park: semwait_woken for a signal (consume the count; the mutex semaphore was already
        // signalled once when the wait registered and must not be signalled again), semwait_timed_out
        // for a deadline fired by the scheduler (ETIMEDOUT, no count to consume).
        uint32_t blocked_on_sem{};
        uint64_t semwait_deadline{};
        bool semwait_woken{};
        bool semwait_timed_out{};

        // The psynch parks (libpthread's pthread_mutex and pthread_cond, BSD 301-305). Both hold the
        // guest address of the pthread object the wait registered against, and they are separate fields
        // because a condition variable wait ends by contending for the mutex it dropped.
        uint64_t blocked_on_psynch_mutex{};
        uint64_t blocked_on_psynch_cv{};

        // Arrival order among the waiters on one address. The thread map is keyed by thread id, which is
        // creation order, so it cannot answer which waiter has been queued longest -- and that is the one
        // xnu hands the mutex to.
        uint64_t psynch_wait_ticket{};

        // What psynch_mutexdrop computed for its successor. Zero means no hand-off is pending: the value
        // always carries EBIT and KBIT, so it can never legitimately be zero.
        uint32_t psynch_mutex_updatebits{};

        // The condition variable park: psynch_cv_woken for a signal or broadcast, psynch_timed_out for a
        // deadline the scheduler fired, and the deadline itself in mach-absolute-time ticks, zero for an
        // indefinite wait. The re-run of the rewound svc reads them to tell the two endings apart -- and
        // a woken re-run must not drop the caller's mutex a second time.
        uint64_t psynch_deadline{};
        bool psynch_cv_woken{};
        bool psynch_timed_out{};

        // __disable_threadsignal is one-way and per-thread: libpthread calls it on a thread that is on
        // its way out, and from then on the thread is not a signal target at all. Measured on 25G76 --
        // pthread_kill to such a thread answers ESRCH for a real signal and for the signal-0 existence
        // probe alike, and a handler that would have run does not.
        bool signals_disabled{};

        // The kport a workqueue worker was spawned with, kept so WQOPS_THREAD_RETURN can park the thread
        // on it: on a real kernel the port is how the workqueue names its threads, and nothing sends to
        // it, so the park ends only if the workqueue path itself wakes the worker.
        uint32_t workqueue_kport{};

        int exit_code{};
        uint64_t executed_instructions{};
        macos_saved_registers saved_regs{};

        bool blocked() const
        {
            return this->blocked_on_port != 0 || this->blocked_on_ulock != 0 || this->blocked_on_sem != 0 ||
                   this->blocked_on_psynch_mutex != 0 || this->blocked_on_psynch_cv != 0;
        }

        // The deadline the scheduler owes this thread, zero when its wait is indefinite or when it is not
        // waiting at all. Every timed park shares one clock, so they have to be comparable.
        uint64_t timed_wait_deadline() const
        {
            if (this->blocked_on_sem != 0)
            {
                return this->semwait_deadline;
            }

            if (this->blocked_on_psynch_cv != 0)
            {
                return this->psynch_deadline;
            }

            return 0;
        }

        // Ends the park the deadline belonged to and leaves the marker its syscall re-run reads.
        void fire_timed_wait()
        {
            if (this->blocked_on_sem != 0)
            {
                this->blocked_on_sem = 0;
                this->semwait_deadline = 0;
                this->semwait_timed_out = true;
                return;
            }

            this->blocked_on_psynch_cv = 0;
            this->psynch_deadline = 0;
            this->psynch_timed_out = true;
        }

        void save(arm64_64_emulator& emu)
        {
            for (size_t i = 0; i < this->saved_regs.x.size(); ++i)
            {
                this->saved_regs.x[i] = emu.reg(macos_thread_detail::general_register(i));
            }

            // x29 and x30 are not contiguous with x0..x28 in unicorn's enum, so they cannot come out of
            // the indexed loop above.
            this->saved_regs.fp = emu.reg(arm64_register::x29);
            this->saved_regs.lr = emu.reg(arm64_register::x30);
            this->saved_regs.sp = emu.reg(arm64_register::sp);
            this->saved_regs.pc = emu.reg(arm64_register::pc);
            this->saved_regs.nzcv = emu.reg(arm64_register::nzcv);
            this->saved_regs.tpidrro_el0 = emu.reg(arm64_register::tpidrro_el0);
            this->saved_regs.tpidr_el0 = emu.reg(arm64_register::tpidr_el0);

            for (size_t i = 0; i < this->saved_regs.v.size(); ++i)
            {
                this->saved_regs.v[i] = emu.reg<macos_vector_register>(macos_thread_detail::vector_register(i));
            }

            this->saved_regs.fpcr = emu.reg<uint32_t>(arm64_register::fpcr);
            this->saved_regs.fpsr = emu.reg<uint32_t>(arm64_register::fpsr);
        }

        void restore(arm64_64_emulator& emu) const
        {
            for (size_t i = 0; i < this->saved_regs.x.size(); ++i)
            {
                emu.reg(macos_thread_detail::general_register(i), this->saved_regs.x[i]);
            }

            emu.reg(arm64_register::x29, this->saved_regs.fp);
            emu.reg(arm64_register::x30, this->saved_regs.lr);
            emu.reg(arm64_register::sp, this->saved_regs.sp);
            emu.reg(arm64_register::pc, this->saved_regs.pc);
            emu.reg(arm64_register::nzcv, this->saved_regs.nzcv);
            emu.reg(arm64_register::tpidrro_el0, this->saved_regs.tpidrro_el0);
            emu.reg(arm64_register::tpidr_el0, this->saved_regs.tpidr_el0);

            for (size_t i = 0; i < this->saved_regs.v.size(); ++i)
            {
                emu.reg<macos_vector_register>(macos_thread_detail::vector_register(i), this->saved_regs.v[i]);
            }

            emu.reg<uint32_t>(arm64_register::fpcr, this->saved_regs.fpcr);
            emu.reg<uint32_t>(arm64_register::fpsr, this->saved_regs.fpsr);
        }
    };
}
