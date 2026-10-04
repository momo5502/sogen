# macOS concurrency

The thread model above [Mach IPC](macos-mach-ipc.md): the cooperative scheduler, the workqueue pool
libdispatch spawns workers from, psynch mutexes and condition variables, `ulock`, and `EVFILT_TIMER`.
Every wait described here shares one virtual clock with the [memory](macos-memory.md) layer's arena
lifetime and with the window server's frame cadence.

## Status

Working: the cooperative scheduler with park/wake for Mach receives, psynch waits, `ulock` and kqueue
timers; the workqueue worker pool with libpthread's own register contract; `EVFILT_TIMER` sourced from
the one scheduler clock; and a per-thread saved-register set that now includes the vector bank. Not
modelled: `psynch_rw_*` (the reader/writer half of the psynch family).

## The thread model and the cooperative scheduler

Threads are cooperative: one runs until it traps into the emulator, and `macos_emulator::
reschedule_away_from_a_blocked_thread` picks the next runnable one. A thread that parks — on a Mach
receive, a psynch wait, a `ulock_wait`, or a kqueue timer with a deadline — has its `pc` rewound onto its
own `svc` so the re-run re-executes the same wait rather than resuming mid-syscall. The scheduler takes
the earliest of three kinds of pending deadline — a thread's own timed wait, an armed `mk_timer`, and a
kqueue timer — from **one** clock, expressed in the same ticks.

An idle run loop is not, by itself, a bug: the scheduler waits for the guest's own counter to reach a
timer deadline instead of firing it immediately, which is what stops an idle `CFRunLoop` from spinning
through billions of instructions doing nothing. Whether a park with
nothing left runnable is a deadlock or a guest waiting for external input is a front-end question,
covered in [`macos-browser.md`](macos-browser.md).

## The workqueue pool and `_pthread_wqthread`'s register contract

libdispatch requests worker threads exclusively through `kevent_qos`/`kevent_id` with a workqueue flag,
and a worker is spawned at `_start_wqthread`, resolved at runtime from the shared cache rather than
hardcoded. The register contract, measured against a live process:

```
_start_wqthread(x0=self, x1=kport, x2=stacklowaddr, x3=keventlist, x4=flags, x5=nkevents)
```

**`x1` is the thread's identity, not a spare handle.** `_pthread_wqthread_setup` stores `x1` in the TSD
slot `pthread_mach_thread_np` reads back (`src/macos-emulator/macos_workqueue.cpp`), so whatever port
sogen hands a worker in `x1` *is* that thread's identity for the rest of its life. Handing a worker a
pool-private port instead of its own real thread port gives it **two identities** — the one `x1` claims
and the one `mach_thread_self()` actually returns — and every comparison of the two, which is exactly
what `os_unfair_lock` and `dispatch_once` do to decide ownership, silently disagrees. This is not a
theoretical hazard: it was the actual defect.

A worker parks back into the pool with `workq_kernreturn(WQOPS_THREAD_KEVENT_RETURN = 0x40, ...)` (plain
kevent path) or `WQOPS_THREAD_WORKLOOP_RETURN = 0x100` (workloop path), handing back the changelist
buffer it was given. `apply_worker_return_changelist` (`src/macos-emulator/syscalls/process.cpp`) is
where every libdispatch timer registration actually arrives — a `workq_kernreturn` that silently drops
its changelist drops every timer the process owns, which was sogen's original behaviour and the reason a
GUI probe's `exit(0)` scheduled 1.5 seconds out never fired.

## psynch mutexes and condition variables

libpthread keeps its own copy of every sequence word inside the `pthread_mutex_t`/`pthread_cond_t` and
compares the kernel's answer against it; a wrong answer is never reported as an error, it makes
libpthread spin or wait on something that will never happen, and the failure surfaces billions of
instructions later somewhere unrelated.

**The single most non-obvious rule: `psynch_mutexwait` is answered with the *dropper's* sequence, not
the waiter's.** Three threads queued on one mutex at lock sequences `0x102`, `0x202` and `0x302` were
each measured answered `0x303` — the sequence the thread that actually dropped the lock carried —
because three separate drops all passed `mgen = 0x300`. Computing the answer from each waiter's own
sequence, the naive reading, would have given `0x103`, `0x203` and `0x303` respectively, and every one of
those is wrong.

A wake that finds no parked waiter is not lost: xnu records a *prepost* so the next wait on that address
takes it without parking, and reports it to userspace with the P bit. sogen keeps the same state, keyed
by the guest address of the pthread object (`macos_process_context::psynch_mutex_preposts` /
`psynch_cv_preposts`). Under sogen's cooperative scheduler a thread cannot be descheduled between
bumping its own userspace sequence word and reaching its `svc`, so the race that produces a prepost on a
real kernel cannot arise the same way — the state is modelled anyway, because discarding the hand-off
turns a legitimate race into a permanent park.

Each of the six psynch calls (`301`–`305`, `312`) joins the same park/wake model as `ulock_wait` and
`__semwait_signal`: a thread records what it is blocked on, the scheduler rewinds its `pc` and runs
someone else, and a wake clears the block and leaves a marker the re-run reads.

## `ulock`

`UL_UNFAIR_LOCK` (opcode 2) and `ULF_NO_ERRNO` are both handled: `ulock_wait` accepts the unfair-lock
opcode on the same compare-and-park path as `UL_COMPARE_AND_WAIT`, and `ULF_NO_ERRNO` is honoured on the
wake side (`src/macos-emulator/syscalls/process.cpp`, around the `MACOS_UL_UNFAIR_LOCK` /
`MACOS_ULF_NO_ERRNO` checks). Getting either of these wrong is silent and severe: a refused wait that
returns `EINVAL` **without yielding the CPU** turns a parked `os_unfair_lock` owner into an infinite spin
inside sogen's own scheduler, because nothing else ever gets to run and release the lock.
libplatform sets `ULF_NO_ERRNO` on every call it makes, so a
handler that ignores the flag and writes a carry-flagged `EINVAL` gets read back as `errno == 1` and
aborts on it.

## `EVFILT_TIMER`, and the single scheduler clock

**libdispatch never asks the kernel for an interval timer.** Every timer registration observed — a
one-shot `dispatch_after` and a 200 ms repeating `dispatch_source` alike — arrives through
`__workq_kernreturn(WQOPS_THREAD_KEVENT_RETURN)` as a *one-shot absolute deadline*
(`NOTE_ABSOLUTE|NOTE_LEEWAY|NOTE_MACHTIME`), never through `kevent_qos` directly and never as an interval.
A repeating source is simply re-armed by the worker at the next deadline. Modelling only the
one-shot-absolute case is
therefore complete, not a simplification.

The units-and-epoch table matters because it is easy to get exactly backwards: without `NOTE_ABSOLUTE`
the value is a relative interval; with it, which clock the deadline is read against depends on
`NOTE_MACHTIME` — set, it is `mach_absolute_time`; clear, it is the **calendar clock**, not
`mach_continuous_time`. A deadline built from `mach_absolute_time() + 130ms` under `NOTE_ABSOLUTE`
without `NOTE_MACHTIME` fired **instantly**, because it was being read against the wrong clock entirely.

A kqueue timer deadline joins the scheduler's one clock alongside a thread's `timed_wait_deadline()` and
an armed `mk_timer`; `macos_emulator::reschedule_away_from_a_blocked_thread()` takes the earliest of the
three. `wait_until` is capped at 20 ms per call, so a 1.5-second deadline is reached in about 75 restful
scheduler steps rather than being fired early or spun on.

## The vector-register finding

`macos_saved_registers` (`src/macos-emulator/macos_thread.hpp`) did not originally save `v0..v31`,
`fpcr` or `fpsr` — only the general-purpose registers, `sp`, `pc`, `nzcv` and the two TLS registers.
AAPCS64 makes the low 64 bits of `v8..v15` callee-saved, so they are live across the `svc` a thread parks
on — the only place sogen ever switches a thread's context — and `v0..v7`/`v16..v31` are live whenever
the switch lands anywhere other than a call boundary, which a parked `svc` is not guaranteed to be.

Measured by comparing the outgoing and incoming saved vector state at every context switch in one
Calculator run: **163 of 203 thread switches (80%) resumed a thread holding another thread's vector
registers**. The fix adds the
full vector bank plus `fpcr`/`fpsr` to `macos_saved_registers`, which is now the current shape of the
struct — see the comment at its definition for the AAPCS64 reasoning above.

This was one of two changes made alongside the [GUI arena work](macos-memory.md); together they reduced
an intermittent guest-heap fault's rate from roughly 5-in-24 runs to about 1-in-30.

## Still open

- **`psynch_rw_*`** (296–300, 306–309), the reader/writer half of the psynch family, is not modelled.
  Nothing measured against a real subject has reached it yet.
- **`pthread_cond_signal_thread_np`'s named-thread wake is approximated.** `psynch_cvsignal` with a
  non-zero `thread_port` releases the longest-queued waiter instead of the named one, and reports itself
  as doing so.
- **`ulock_wait` timeouts are not modelled.** A non-zero timeout still parks the thread rather than lying
  about an immediate return, but nothing currently wakes it when the timeout expires — a guest whose
  timed `ulock_wait` is supposed to give up and retry instead waits forever
  (`src/macos-emulator/syscalls/process.cpp`, `sys_ulock_wait`).
- **Every `kevent` filter except `EVFILT_TIMER`, `EVFILT_MACHPORT`, `EVFILT_WORKLOOP` and `EVFILT_USER`
  registers and never fires.** `EVFILT_READ`, `WRITE`, `AIO`, `VNODE`, `PROC`, `SIGNAL`, `FS`, `VM`,
  `MEMORYSTATUS` and `EXCEPT` are all accepted and named once in a warning rather than silently dropped,
  but sogen has no event source behind any of them
  (`src/macos-emulator/syscalls/kqueue.cpp`, `report_unmodelled_filter_once`); `EVFILT_PROC` and
  `EVFILT_MEMORYSTATUS` are the two a real application has already reached.
- **The workqueue-park deadlock site does not yet know about host input.** The mach-receive park does
  (see [`macos-browser.md`](macos-browser.md)), but a launched GUI application can just as easily come to
  rest on a dry workqueue pool instead, and that site needs the same two-line change. It is in a file
  another piece of work owns and has not yet landed.
- **A second, unidentified source of guest-heap corruption remains.** Separating the GUI arena from the
  guest's mmap arena (`macos-memory.md`) closed the pixel-filled corruption channel, but three faults in
  twelve runs still showed an all-zero per-thread malloc cache with no emulator-claimed range covering
  the address — the candidates, and the order they should be tested, are under "Still open" in
  [`macos-memory.md`](macos-memory.md).
