# Multi-vCPU Support — Design

Status: in progress on branch `multi-vcpu`

- [x] Phase 0a — capability query, `vcpu_count` setting, `--vcpus`, hard-error validation (4aa27ba1)
- [x] Phase 0b — `x86_64_cpu` extraction (5611f1d4); hook callbacks carry the triggering CPU
      (a68338eb); `vcpu_context` replaces `process_context::active_thread`, `CURRENT_THREAD`
      resolution takes the calling thread explicitly, snapshot format unchanged (9d6be10b)
- [x] Phase 1 — WHP drives N VPs: per-VP `whp_vcpu` objects, parameterized VP index,
      `partition_mutex_` (shared_mutex, copy-then-invoke callback discipline),
      `ProcessorCount = N`, factory takes `vcpu_count` (eef2e558). Clang-tidy clean.
- [~] Phase 2 — BEL + vcpu_worker scheduler. Done: `kernel_lock` (owner-tracking,
      non-recursive, assert-based interior checks) acquired at every entry point (hook
      callbacks, UI event delivery, scheduler loop — released across guest execution
      and idle sleeps); logger print mutex; startup conflict validation (instruction
      precision / relative time / instruction budgets / gdb at N>1); one host worker
      thread per vCPU (`vcpu_worker`) with per-vCPU thread ownership (a guest thread
      loaded on one vCPU is skipped by the others; detached when the vCPU goes idle);
      timer thread kicks all running vCPUs; `stop()` cancels all vCPUs; WHP per-vCPU
      deferred TLB flush (flag + cancel, applied at run-loop top) replacing the
      cross-vCPU CR3 rewrite. N=1 keeps its proven inline loop (workers only spawn at
      N>1); N=1 + all tests unaffected.
- [x] Phase 3 (core) — **parallel execution is stable**. The Threads-test race was a
      spurious access violation: under multiple vCPUs a page that is backed with permissions
      that allow the access still faulted because this vCPU held a *stale translation* (a
      peer had just mapped/committed/reprotected it — e.g. ntdll `.mrdata`/`.00cfg`/`.rsrc`
      flipping RO↔RW during loader init). The escalated fault delivered a bogus 0xC0000005.
      Fix: `try_repair_spurious_fault` in all three WHP fault paths re-establishes the
      mapping + flushes this vCPU's TLB + retries whenever the page is backed and its
      intended permissions allow the operation (guard/no-access pages map with permission
      `none`, so genuine violations are still delivered); a per-vCPU (page,rip) retry
      counter (cap 256) rides out concurrent re-clears at high vCPU counts without looping.
      Result: `--vcpus 2` 70/70, `--vcpus 4` 27/28, `--vcpus 8` ~125/126 smoke runs pass;
      N=1 and the 23 unit tests unaffected. Remaining Phase 3 (non-blocking): cross-vCPU
      kicks for alert/APC/suspend/terminate targeting a *running* thread (currently take
      effect at the next ~20ms timer preemption); NtGet/SetContextThread force-save;
      stop-the-world snapshot quiesce (snapshots still N=1); guest CPU count tied to
      vcpu_count. Details of the earlier register/dispatch fixes below.
- [~] Phase 3 (earlier fixes) — Fixed the register-routing bug that made every
      syscall on a non-zero vCPU touch vCPU 0's registers: `syscall_context::emu` (and the
      argument/register helpers, `callback_frame`, exception dispatch, rdtsc/rdtscp/
      interrupt/violation hooks) now use the acting vCPU's `x86_64_cpu` instead of
      `windows_emulator::emu()` (the vCPU-0 facade). Guest memory is shared, so
      `x86_64_cpu` gained an implicit conversion to the shared `memory_interface&`. The
      legacy `current_thread()` observer API is made correct via `scoped_dispatch`: a
      `windows_emulator::dispatch_vcpu_` member set at each handler entry under the kernel
      lock (race-free because the BEL serializes handlers — not a global or thread-local).
      Also fixed a WHP premature-termination bug: `flush_virtual_address_mappings` cancels
      other running VPs (`WHvCancelRunVirtualProcessor`) without setting `stop_requested_`,
      and `run()`'s Canceled handler returned (instead of restarting) when rip had advanced
      — so a vCPU cancelled for a peer's page-table change returned to the worker, which saw
      `!switch_thread` and ended the whole run ("Emulation terminated without status"). A
      cancel without `stop_requested_` is now always a TLB-flush re-entry, never a stop.
      With these fixes N>1 runs the entire smoke suite correctly through ~15 tests and the
      verbose run reaches a clean `NtTerminateProcess` (status 0). The **Threads** test
      still fails under `-s` (silent, fastest/most-parallel) — a genuine timing race
      (Heisenbug: host logging serializes enough to hide it). A guest thread raises an
      unhandled exception (WER is contacted) under contention. Prime suspect: the
      partition-global EPT single-step dance in the WHP section-first-execution hooks (two
      vCPUs flipping/​restoring exec on the same shared page — the accepted-race from §4;
      may not be benign under real contention) — but `--whp-exec-hook int3` fails
      identically, so the exec-hook mechanism was **ruled out**. Fixed a real off-lock
      race since: KUSD MMIO reads (`kusd_mmio::read`/`update`) run on the worker thread
      during guest execution (kernel lock released) and were unguarded, so two vCPUs
      reading the time page tore `kusd_`; now guarded by an internal mutex. That, plus the
      cancel-return fix, took the `--vcpus 2` smoke suite from near-always-failing to
      ~6/8 passing (full suite completes, clean `NtTerminateProcess`).
      Post-mortem exception tracer added (env `SOGEN_TRACE_EXCEPTIONS=1`, dumped after the
      run so it doesn't perturb the race): it pinpointed the crash as a spurious access
      violation (0xC0000005) on a page the memory manager reports **committed** — the guest
      page-table entry is absent on the faulting vCPU. Deterministic fault address
      (ntdll+0x1ea598, an image `.data`/COW page) but intermittent by timing. Added a
      stale-TLB retry: a not-present #PF whose guest PTE is actually present flushes the
      vCPU's TLB and resumes instead of delivering a bogus AV. That only helps when the PTE
      is present; the residual case is a **committed page whose eager WHP mapping is absent
      on the faulting vCPU** — likely the image/COW mapping path (not the eager
      `map_memory`) not being established/flushed across vCPUs. Next: instrument the WHP
      violation fall-through to log GvaValid + mapped_pages_ state + is_virtual_mapping_present.
      Root-caused it: guest page faults escalate to WHP's *unrecoverable-exception* exit
      (the guest IDT can't dispatch #PF), and that path read CR2 and delivered a memory
      violation **without** any spurious-fault check — so a fault on an actually-backed
      page (peer map/commit, or a transient protection/COW remap on another vCPU clearing
      this vCPU's PTE) became a bogus AV. Fix: `try_repair_spurious_fault` (re-establishes
      the mapping + flushes this vCPU's TLB + retries, with a per-vCPU (page,rip) guard so
      genuine violations — null derefs, wrong-permission pages — are still delivered after
      one cycle) now runs in all three fault paths (`handle_exception` #PF,
      `handle_memory_access` GpaUnmapped, `handle_unrecoverable_exception`). This lifted
      the `--vcpus 2` smoke pass rate to ~16/24 (was ~1/2). A speculative flush in
      `apply_memory_protection` was tried and reverted (no clear help; Hyper-V handles
      EPT-level coherency; added churn).
      **Residual**: ~1/3 of silent `--vcpus 2` runs still fail at the **Threads** test — the
      repair fires but the page is sometimes concurrently re-cleared (guard trips → AV
      delivered), pointing at a tighter map/unmap/protect race on a shared page, or a
      guest-level loader-lock / .mrdata race. Still Heisenbug-y (vanishes under hot-path
      logging); the env-gated post-mortem tracer (`SOGEN_TRACE_EXCEPTIONS=1`) is the tool.
      Remaining Phase 3: that race; cross-vCPU kicks; stop-the-world for snapshots; guest
      CPU count = vcpu_count. Remaining Phase 2 polish: ready_cv instead of sleep-poll;
      device pump on ticks; N=1 boot benchmark; clang-cl -Wthread-safety annotations.
- [ ] Phase 3 — cross-vCPU correctness
- [ ] Phase 4 — stress testing + contention profiling
- [ ] Phase 5 — KVM parity, linux-emulator

Scope: WHP backend first, KVM second; windows-emulator first, linux-emulator second.
Goal: run guest threads truly in parallel — one host thread drives one vCPU — instead of
cooperatively multiplexing all guest threads onto a single vCPU.

---

## 1. Where we are today

The whole stack is built around **one vCPU, one host execution thread**:

- Each backend instance *is* one vCPU. WHP hardcodes `constexpr UINT32 vp_index = 0`
  (`whp_x86_64_emulator.cpp:29`) into every `WHvCreateVirtualProcessor` /
  `WHvRunVirtualProcessor` / `WHvGet/SetVirtualProcessorRegisters` /
  `WHvCancelRunVirtualProcessor` call, and sets partition `ProcessorCount = 1`
  (`:1866`). KVM mirrors this (`kvm_x86_64_emulator.cpp:62`, one `vcpu_fd_`).
- Guest threads are multiplexed cooperatively: a context switch serializes the entire
  register file into `emulator_thread::last_registers` (`emulator_thread.hpp:335`) via
  `save_registers()`/`restore_registers()` and swaps another thread's blob into the single
  vCPU (`switch_to_thread`, `windows_emulator.cpp:385`).
- Scheduling lives in `windows_emulator::start()` (`windows_emulator.cpp:1023`):
  round-robin (`switch_to_next_thread`, `:442`), preempted either per-instruction /
  per-basic-block (unicorn/icicle) or by a 20 ms interrupt thread that calls
  `emu().stop()` → `WHvCancelRunVirtualProcessor` (WHP/KVM, `:1052-1068`).
- **Almost nothing is synchronized**, because it never had to be. The entire concurrency
  surface today: `switch_thread_` / `should_stop` atomics, the
  `memory_manager::layout_version_` counter, the interrupt thread, and one web-UI mutex.

State a second concurrently-executing vCPU would race on, ranked by blast radius:

| # | Shared state | Location |
|---|--------------|----------|
| 1 | The single vCPU register file (save/restore blob swap) | `emulator_thread.hpp:335,379-388` |
| 2 | Scheduler state: `threads`, `active_thread`, `thread_handles_by_id` | `process_context.hpp:468,483-484` |
| 3 | All `handle_store` object tables (events, mutants, files, sections, windows, …) + non-atomic ref counts | `handles.hpp:184,147`, `process_context.hpp:449-467` |
| 4 | `memory_manager::reserved_regions_` (plain `std::map`, mutated by NtAllocate/Protect/FreeVirtualMemory) | `memory_manager.hpp:155` |
| 5 | WHP partition tables: `mapped_pages_`, `guest_pages_by_gpa_`, `free_guest_gpa_indices_`, `page_table_views_`, `mmio_regions_`, all hook maps | `whp_x86_64_emulator.cpp:1809-1839` |
| 6 | Wait/APC bookkeeping written cross-thread (NtQueueApcThread, NtAlertThreadByThreadId target *other* threads) | `syscalls/thread.cpp:555,974` |
| 7 | KUSD (`kusd_mmio::update()` recomputed on every read) | `kusd_mmio.cpp:157-179` |
| 8 | Devices (`io_device::work()` polled from the CPU thread, `block_mutation` reentrancy hack) | `windows_emulator.cpp:301-310`, `io_device.hpp:190` |
| 9 | Hook containers in every backend (unlocked maps/vectors, hooks deleted from inside callbacks) | e.g. `whp_x86_64_emulator.cpp:1826-1833`, `windows_emulator.cpp:748-752` |

Two facts make this project tractable:

1. **Guest thread state is already decoupled from the vCPU.** The register-blob model
   (`last_registers`) is exactly what an N-vCPU scheduler needs — a thread's context can
   be restored onto *any* vCPU.
2. **WHP/KVM already keep the expensive state at partition scope.** EPT/GPA mappings,
   page tables, MMIO regions and hooks are per-partition, which is precisely what
   multiple vCPUs must share. The per-vCPU part (registers, run/cancel, single-step
   state) is small and cleanly separable.

---

## 2. The two core design decisions

### Decision 1 — N vCPUs + a real scheduler (not one host thread per guest thread)

Two possible execution models:

- **(a) 1 guest thread = 1 vCPU = 1 host thread.** No scheduler at all; blocked guest
  threads park on host condition variables. Rejected: guest processes create dozens to
  hundreds of threads (worker factories, thread pools); WHP `ProcessorCount` is fixed at
  partition setup and vCPUs are heavyweight; guests also behave badly when the apparent
  CPU count is huge.
- **(b) N vCPUs (N = configured processor count, e.g. 4), M guest threads multiplexed
  onto them by a scheduler — like a real kernel.** Each vCPU is driven by one dedicated
  host worker thread: pick a ready guest thread, restore its register blob onto *this*
  vCPU, run until yield/preempt/wait, save the blob back, repeat.

**(b) is the design.** It preserves the existing, proven `save/restore` context-switch
machinery, keeps the guest-visible CPU count consistent with what we already fake
(`fake_environment_config::number_of_processors = 4`, `windows_emulator.hpp:85`), and
degrades gracefully to today's behavior at N=1.

### Decision 2 — a Big Emulator Lock (BEL) for the "kernel", parallel guest usermode

The windows-emulator layer is effectively a kernel: `process_context`, handle stores,
memory manager, devices, and ~hundreds of syscall handlers, all written assuming
serialized execution. Options:

- **Fine-grained locks per structure.** Rejected as the starting point: syscall handlers
  assume atomicity *across* structures (e.g. `NtWaitForMultipleObjects` reads several
  object tables and thread wait state together; `is_thread_ready` /
  `consume_object_signal` rely on "nothing else runs while I decide"). Retrofitting
  per-structure locks means auditing every handler for cross-structure invariants and
  invites deadlocks. This is a months-long audit with a long bug tail.
- **A dedicated kernel thread (actor model)** — vCPU exits post messages to one kernel
  thread and block. Rejected: same serialization as a lock but adds two thread handoffs
  of latency to every syscall.
- **One global kernel mutex (the BEL).** Guest code executes with the lock *released* —
  that is where the parallelism win is, since programs spend the vast majority of time in
  usermode. Every VM exit that enters emulator C++ code (syscall, exec-hook, MMIO,
  exception, memory violation) acquires the BEL, handles the exit, releases it, resumes
  the guest.

**The BEL is the design.** It is the same evolution QEMU took (single-threaded TCG → the
"Big QEMU Lock" + MTTCG) and it keeps virtually all existing windows-emulator code
correct without modification, because everything that touches shared state already runs
in exit context. Contention is bounded by syscall/exit frequency, which profiling has
shown to be far lower than guest execution time for the workloads we care about (games).
Lock granularity can be reduced *later*, guided by profiling, without another
architecture change (§9).

Guest-side synchronization needs no help from us: guest usermode interlocked operations
act on real shared memory through the shared GPA space and work natively across vCPUs;
their contention slow paths are syscalls, which the BEL serializes.

---

## 3. Core interface changes (`src/emulator`)

Split "the machine" from "a CPU". Today `emulator` multiply-inherits
`cpu_interface + memory_interface + hook_interface` (`emulator.hpp:12`). New shape:

```
emulator (machine: memory_interface + hook_interface + vCPU registry)
 ├── supports_multiple_vcpus() const -> bool
 ├── vcpu_count() const -> size_t              // configured at creation
 └── get_cpu(size_t index) -> x86_64_cpu&

x86_64_cpu (per-vCPU: cpu_interface + typed register helpers + machine access)
 ├── start(count) / stop()
 ├── read/write_raw_register, reg<T>(), save_registers()/restore_registers()
 ├── memory access, delegated to the shared machine
 └── index() const
```

Key points:

- **Explicit CPU context — no thread-local state.** Each vCPU worker thread owns its
  `x86_64_cpu` for the lifetime of the run and passes it explicitly down every call
  chain. Nothing resolves "the current CPU" implicitly; there is no ambient state to get
  wrong when code runs on an unexpected thread.
- **Hook callbacks gain the triggering CPU as a parameter.** Every callback type in
  `hook_interface.hpp:44-55` gets `cpu_interface&` (typed `x86_64_cpu&` at the arch
  layer) as its first argument; the backend invokes callbacks with the vCPU whose exit
  is being handled, always on that vCPU's worker thread.
- **`x86_64_cpu` is "a CPU's view of the machine".** Registers and run control are
  native per-VP; the memory surface delegates to the shared machine. Today's syscall
  layer uses one `x86_64_emulator&` for both registers and memory
  (`syscall_context::emu`), so swapping that member — and the exit-handling paths — to
  `x86_64_cpu&` keeps the hundreds of `c.emu.reg(...)` / `c.emu.read_memory(...)` sites
  compiling unchanged while making the target CPU explicit. The typed register helpers
  in `typed_emulator.hpp` are factored into a mixin `x86_64_cpu` reuses.
- **Capability query:** the emulator interface exposes
  `bool supports_multiple_vcpus() const`. WHP returns `true`; unicorn and icicle return
  `false`; KVM returns `false` for now (the hypervisor supports it, but multi-vCPU KVM
  is deferred to Phase 5 and reports unsupported until implemented).
- **Creation:** `create_x86_64_emulator(backend_type, emulator_creation_settings{ .vcpu_count })`
  (`backend_selection.hpp:10-19`). `vcpu_count` defaults to `1`. Requesting more than one
  vCPU on a backend where `supports_multiple_vcpus()` is `false` is a hard error at
  creation time — no silent clamping.
- **Thread-safety contract, stated at the interface:**
  - Per-vCPU operations (`start`, `stop`, register access, save/restore) are safe to call
    concurrently on *different* vCPUs; register access to a vCPU is only valid from its
    owning worker thread or while it is known-stopped.
  - Memory mutation (`map/unmap/protect`) and hook add/remove require external
    serialization by the caller (the BEL provides it). Backends additionally guard their
    internal partition tables (§4) because some mutations happen on exit-handling paths
    inside `start()`.
  - `stop()` on any vCPU remains callable from any thread (this is what
    `is_stop_thread_safe()` already promises for WHP/KVM) — it becomes the **IPI/kick
    primitive** (§5.2).

`memory_manager` stays a single shared instance (it already is, held by value on
`windows_emulator`); its metadata is BEL-protected because it is only mutated from
syscall/exit context.

---

## 4. WHP backend changes

Reference: everything below is in `src/backends/whp-emulator/whp_x86_64_emulator.cpp`.

**Per-vCPU (new `whp_vcpu` struct, one per index):**
- `virtual_processor_handle` (`:96-118`) — created per index; partition
  `ProcessorCount = N` in `configure_partition()` (`:1866`).
- Run state: `stop_requested_`, `run_active_` (`:1820-1821`).
- Single-step machinery: `pending_execution_step_` (`:1822`), `pending_mmio_step_`
  (`:1837`), `deferred_patched_breakpoint_` / `deferred_execution_page_` (`:1834-1835`).
- Exit context buffer, per-vCPU register setup (LSTAR → shared HLT syscall page, GDT/IDT
  bases can point at the same shared structures; the syscall intercept page `:1948-1957`
  is read-only and shared).
- All `WHvRunVirtualProcessor` / `WHvCancelRunVirtualProcessor` /
  `WHvGet/SetVirtualProcessorRegisters` calls parameterized on the vCPU index instead of
  the file-scope `vp_index`.

**Partition-shared, needs an internal `partition_mutex_`:**
`mapped_pages_`, `guest_pages_by_gpa_`, `free_guest_gpa_indices_`, `page_table_views_`,
`pml4_gpa_`, `next_internal_gpa_` (`:1809-1819`), `mmio_regions_` +
`mmio_read_grace_deadlines_` (`:1836-1838`), all hook maps + `next_hook_id_` +
`syscall_hook_` (`:1824-1839`), `patched_execution_breakpoints_` (`:1833`).

Why the backend needs its own lock even with the BEL: exec-hook and MMIO exit handling
(`handle_memory_access` → `remap_hook_page` / `remap_pages`, `:2174-2291`) mutates these
tables *inside* `start()`, i.e. while the caller does **not** hold the BEL. Locking
discipline to avoid inversion:

- Lock order is **BEL → partition_mutex**, never the reverse.
- The backend must never hold `partition_mutex_` while invoking a user callback
  (callbacks acquire the BEL). The existing copy-callbacks-into-a-local-vector-then-invoke
  pattern (`:3154`, `:3276`) already supports this — take the lock only around container
  access.
- `WHvMapGpaRange`/`WHvUnmapGpaRange` are partition-level; Hyper-V performs its own EPT
  invalidation across running VPs. We still serialize *our* calls under
  `partition_mutex_` so the bookkeeping and the hypervisor state cannot diverge.
  (Verify empirically in Phase 1 that remapping while other VPs run behaves; if not,
  unmap/protect must briefly kick all vCPUs — a stop-the-world helper we need anyway,
  §5.2.)

**The exec-hook single-step window (known, accepted race):**
Section-first-execution hooks strip EPT exec from a page; on the fault, the handler fires
callbacks, temporarily restores exec, single-steps via TF, and re-protects
(`effective_page_permissions :2162`, `arm_execution_single_step :2479`,
`complete_execution_step :2527`). EPT permissions are partition-global — while vCPU A
steps over a hooked page, vCPU B could execute that page without faulting. Consequences:

- For **section-first-execution** hooks this is benign: the callback semantic is
  "fire once on first execution", enforced under the BEL by the `first_execute` flag
  check in `track_section_first_execution` (`windows_emulator.cpp:741`). A near-miss
  duplicate fault is filtered; a missed extra execution is irrelevant.
- For **precise breakpoints** (gdb stub, debugger, int3-patched breakpoints — the 0xCC
  byte is removed from shared memory during the step, `:2408-2477`) the race is *not*
  acceptable. Resolution: when precise execution hooks / breakpoints are active, either
  run with `vcpu_count = 1` (the analyzer/debugger default) or use the stop-the-world
  helper around the step window. Do not attempt per-vCPU EPT views — WHP (and KVM
  memslots) have no per-VP GPA mappings.

**Not supported under multi-vCPU (unchanged constraints):** instruction counting
(`supports_instruction_counting()` is already `false`, `:1734`), `start(count)` with
exact budgets, instruction-precision mode. Combining these with `vcpu_count > 1` is a
hard error at startup (decision: no silent clamping, consistent with the
unsupported-backend rule).

---

## 5. windows-emulator changes

### 5.1 The scheduler

`windows_emulator::start()` (`windows_emulator.cpp:1023`) becomes a coordinator:

```
start():
    spawn N vcpu_worker threads (N = min(settings.vcpu_count, emu().vcpu_count()))
    main thread: pump UI events + user timers, until exit/stop     // SDL wants the main thread
    join workers

vcpu_worker(cpu):
    loop:
        lock BEL
        thread = pick_ready_thread()            // round-robin over process.threads, skipping
                                                // threads currently running on another vCPU
        if none: wait on ready_cv (with timeout for timed waits / host-condition polls)
        thread->restore(cpu)                    // blob -> this vCPU
        mark thread running-on(cpu); unlock BEL
        cpu.start()                             // guest runs in parallel, BEL released
        lock BEL
        thread->save(cpu)                       // exits for syscalls etc. acquired BEL internally
        handle wait state / APCs / termination; mark not-running
        unlock BEL
```

- **`vcpu_context` replaces the global "current thread" notion.** A per-vCPU struct
  owned by `windows_emulator` (in a plain vector, no globals or thread-locals):

  ```cpp
  struct vcpu_context
  {
      x86_64_cpu& cpu;
      emulator_thread* active_thread{};   // replaces process_context::active_thread
      std::atomic_bool switch_thread{};   // per-vCPU yield request (timer thread writes it)
      bool apc_alertable{};
  };
  ```

  `process_context::active_thread` (`process_context.hpp:484`) and the global
  `switch_thread_` go away. `windows_emulator::current_thread()` is removed; every path
  that needs it — syscall handlers (via `syscall_context`), exception dispatch, user
  callback dispatch, hook lambdas — receives the `vcpu_context&` explicitly. Hook
  callbacks carry the triggering `x86_64_cpu&` (§3), and `windows_emulator` maps
  `cpu.index()` to its `vcpu_context` in O(1). This is the main churn cost of avoiding
  ambient state: a wide but compiler-guided, purely mechanical refactor (delete
  `current_thread()`, follow the errors).
- `should_stop` stays a single member atomic on `windows_emulator`.
- `ready_cv` is signaled whenever a thread becomes ready: object signaled
  (`NtSetEvent`, `NtReleaseMutant`, …), APC queued, alert delivered, thread created or
  resumed. All of these already run under the BEL, so the wakeup is race-free.
- **Time-slicing:** one timer thread (evolution of today's interrupt thread,
  `windows_emulator.cpp:1052-1068`) kicks (`cpu.stop()`) every running vCPU whose slice
  expired — but only when `ready_threads > running_vcpus`, so an under-committed guest
  never pays preemption exits.
- **Device pump:** `perform_context_switch_work()` (`:301-310`, devices `work()`,
  thread reaping) moves to a scheduler tick executed under the BEL by whichever worker
  hits it next (deadline-based), preserving the `block_mutation` invariant because the
  BEL serializes it.
- **`start(count)`** (instruction budgets, used by the analyzer's `-c` and gdb
  single-step) is incompatible with `vcpu_count > 1` — hard error at startup.
- **N=1 runs through the same scheduler** (decision): a single `vcpu_worker` with an
  uncontended BEL replaces the legacy loop entirely, so there is exactly one scheduling
  code path. Phase 2 must benchmark N=1 boot wall-time against main before merging.

### 5.2 Cross-vCPU operations need a kick (IPI equivalent)

Today, targeting *another* thread always mutates a parked thread. Now the target may be
executing on another vCPU. All of these run under the BEL and use `cpu.stop()` on the
target's vCPU as the kick:

- `NtAlertThreadByThreadId` / `NtQueueApcThread` (`syscalls/thread.cpp:555,974`): set the
  flag/queue the APC (BEL makes it safe), then kick the target's vCPU so an alertable
  wait or APC delivery is prompt.
- `NtSuspendThread` / `NtTerminateThread`: kick, and the worker parks the thread when it
  observes `suspended`/`exit_status` on the next exit.
- `NtGetContextThread` / `NtSetContextThread` on a *running* thread: kick the target vCPU,
  wait (under BEL, via a condition) until the worker has saved the blob, then operate on
  `last_registers` — the mechanism that already exists for parked threads.
- **Stop-the-world helper** (needed for snapshots, precise-breakpoint stepping, possibly
  unmap): set a global pause flag, kick all vCPUs, wait until every worker has parked
  its thread, run the critical action, release.

### 5.3 What the BEL does NOT have to fix

- Guest-vs-guest memory races: same as real hardware; guest's problem.
- Syscall handlers reading guest memory that another vCPU concurrently writes: same
  TOCTOU exposure a real kernel has; handlers already copy-in/copy-out via
  `emulator_object`.
- KUSD: reads are MMIO exits → BEL-serialized `update()` (`kusd_mmio.cpp:157`). No change
  needed initially.

### 5.4 Smaller items

- `executed_instructions_` (`windows_emulator.hpp:114`) → atomic (or per-vCPU counters
  summed on read).
- `handle_store` ref counts (`handles.hpp:147`) stay non-atomic — all mutation is under
  the BEL. Document that invariant on the class.
- `logger` needs an internal mutex (interleaved lines otherwise).
- Guest-visible CPU identity (decision: **tied to `vcpu_count`**): the vCPU count drives
  `PEB.NumberOfProcessors` (`process_context.cpp:360`),
  `KUSER_SHARED_DATA.ActiveProcessorCount` (`kusd_mmio.cpp:72`) and derived affinity
  masks; `fake_env.number_of_processors` becomes an explicit override for anti-analysis
  experiments. `NtGetCurrentProcessorNumberEx` (`thread.cpp:916`) returns the index of
  the vCPU that took the syscall (`c.cpu.index()`). Affinity/priority remain no-ops.
- **Serialization/snapshots** (decision: **restricted to N=1 in v1**):
  `save_snapshot()`/`serialize()` at `vcpu_count > 1` is an error until the
  stop-the-world quiesce lands in Phase 3. Per-thread register blobs already serialize;
  the interrupt-thread atomics currently serialized (`windows_emulator.cpp:1315`) become
  per-vCPU state that resets on restore.
- **gdb-stub / debugger** (`x86_64_gdb_stub_handler.hpp`, `debug_session.cpp`):
  incompatible with `vcpu_count > 1` — hard error when combined. Multi-vCPU debugging
  (thread ids per vCPU, stop-the-world stepping) is a later, separate feature.
- **UI events:** `deliver_raw_input` / `handle_ui_event` (`windows_emulator.cpp:1103`)
  are called from the main thread — they take the BEL.

---

## 6. Hook system thread safety

Findings: hook containers are unlocked in every backend; hooks are legitimately deleted
from inside their own callback (`track_section_first_execution`,
`windows_emulator.cpp:748-752`); only icicle has deferred add/remove
(`icicle_x86_64_emulator.cpp:468,645`).

Rules under the new model:

1. **Registration/removal is BEL-serialized by contract** (§3). All current
   install/remove sites (all in `setup_hooks()` `windows_emulator.cpp:823` and the
   section-first-exec paths) already run in exit context or before workers start.
2. **Dispatch containers get the backend `partition_mutex_`** for the exit-handling
   paths that consult them concurrently from multiple workers
   (`handle_execution_hook :3144`, `handle_instruction_exit :3015`,
   `handle_memory_access :3190`, `handle_syscall_halt :1959`). Copy-then-invoke (already
   the pattern at `:3154/:3276`) keeps callbacks outside the lock.
3. **Delete-from-callback keeps working**: the callback runs holding the BEL; removal
   mutates the container under `partition_mutex_`; other workers only read the container
   under the same lock and iterate over their local copy. A deleted hook may still fire
   once from another vCPU's already-taken copy — callbacks that care (section-first-exec)
   are idempotent-by-check under the BEL.
4. `scoped_hook` (`scoped_hook.hpp`) needs no change — destruction happens on
   BEL-holding paths.
5. The **syscall hook** stays special-cased: `syscall_hook_` (`:1839`) is written once at
   setup before workers exist; concurrent dispatch is read-only → no lock on the hot
   path.

---

## 7. Concurrency discipline — race freedom and deadlock avoidance

A general design rule for this project applies throughout: **no mutable global state,
and `thread_local` counts as global.** Every piece of shared state is a member of an
object with a clear owner, and the executing CPU/thread identity is always passed
explicitly (§3, §5.1). This is not just style — it makes the ownership taxonomy below
*checkable*, because there is no ambient state that can be touched from the wrong
thread invisibly.

### 7.1 Data-race freedom: every object gets exactly one ownership class

Race freedom is not achieved by sprinkling locks; it is achieved by classifying **all**
mutable state into one of six classes and enforcing each class's access rule:

| Class | Rule | Examples |
|-------|------|----------|
| **vCPU-local** | Touched only by its owning worker thread. The *only* cross-thread operation is `stop()` (the kick). | `x86_64_cpu` registers/run state, WHP per-VP step state, `vcpu_context` (except its atomic) |
| **Kernel state (BEL domain)** | Touched only while holding the BEL. | `process_context` + all handle stores, `memory_manager` metadata, `module_manager`, devices, scheduler queues, `emulator_thread` fields visible across threads |
| **Partition state (backend)** | Touched only while holding the backend's `partition_mutex_`. | WHP GPA/page tables, hook dispatch containers, MMIO regions |
| **Frozen after setup** | Written only before vCPU workers spawn; read-only afterwards, safe to read concurrently without locks. | syscall handler table (`syscall_dispatcher::handlers_`), module exports, settings, the syscall hook pointer |
| **Designated atomics** | Individually documented, each with its ordering semantics. | `should_stop`, `layout_version_`, per-vCPU `switch_thread`, instruction counters |
| **Guest memory** | Raced by design, like physical RAM. Kernel code must copy-in/copy-out (`emulator_object` already does) and never assume stability across two reads. | all guest pages |

Enforcement, so the discipline survives contact with reality:

- **Owner-tracking lock wrappers.** The BEL and `partition_mutex_` are wrapped in a
  small class that records the owning `std::thread::id` (a member of the wrapper — not
  thread-local). Every core mutator asserts ownership at entry:
  `handle_store::store/erase`, all `memory_manager` mutators,
  `process_context::create_thread/terminate_thread`, device container mutation. Debug
  builds abort on violation; the check is cheap enough to keep enabled in release
  during the bring-up phases.
- **Acquire at the boundary, assert in the interior.** The BEL is taken exactly once
  per entry point (VM-exit handler, UI event delivery, scheduler loop iteration).
  Interior code never acquires it — it asserts it is held. No `recursive_mutex`:
  attempted nested acquisition is a bug the owner assert catches immediately, instead
  of a design smell a recursive lock would paper over.
- **Static checking:** clang `-Wthread-safety` annotations (`GUARDED_BY(bel_)` /
  `REQUIRES(bel_)`) on the core structs. A local clang-cl install (clang 21,
  `x86_64-pc-windows-msvc`) is available for development; because clang-cl is
  MSVC-compatible it builds the WHP backend too, so the analysis covers the actual
  multi-vCPU code paths on Windows. No dedicated preset needed — override the compiler
  at configure time (`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` plus
  `-Wthread-safety`) for development/verification builds, and keep them warning-clean
  during Phases 2-3. The annotation macros compile to nothing on MSVC.
- **Dynamic checking:** ThreadSanitizer on the Linux/KVM build once Phase 5 lands
  (TSan is not available on Windows, including under clang-cl); on Windows, the Phase 4
  guest stress test plus the owner asserts are the practical net.
- **Review rule:** no new static/global/thread_local mutable state, ever; every new
  member field belongs to exactly one class above, stated in a comment when not
  obvious from context.

### 7.2 Deadlock avoidance: a fixed, tiny lock hierarchy

The design deliberately has **three lock levels, total**, strictly ordered. A thread
holding a lock may only acquire locks of a strictly higher level:

```
L0  BEL (windows_emulator kernel mutex)     — outermost
L1  partition_mutex_ (one per backend)      — may be taken under the BEL, never above it
L2  leaf mutexes (logger)                   — never call out while held
```

Rules that make the hierarchy sufficient:

1. **The BEL is never held while executing guest code** (`cpu.start()`) or while
   blocking on host I/O. All waiting under the BEL goes through condition variables
   bound to it (`ready_cv`, the stop-the-world barrier, the force-save wait in
   `NtGetContextThread`) — a CV wait atomically releases the BEL, so the thread being
   waited *for* can always acquire it and make progress. That closes the classic
   "A holds the lock waiting for B, B needs the lock" cycle by construction.
2. **`partition_mutex_` is a bookkeeping lock only**: held around container/EPT table
   access, never across user callbacks (callbacks acquire the BEL → would invert L0/L1),
   never across `WHvRunVirtualProcessor`, never across anything that blocks. The
   existing copy-callbacks-then-invoke pattern (`whp_x86_64_emulator.cpp:3154,3276`)
   already has the right shape.
3. **Kicks are non-blocking.** `WHvCancelRunVirtualProcessor` / `pthread_kill` never
   block, so signaling another vCPU is safe under any lock. Anything that must *wait*
   for the kicked vCPU to react does so via a CV on the BEL (rule 1), never by spinning
   with a lock held.
4. **Leaf locks never call out.** The logger mutex wraps formatting+write only.
5. **Adding a mutex requires assigning it a level** in the hierarchy (documented next
   to the BEL). The expected steady state is that no one ever needs to: contention
   fixes in Phase 4 should prefer sharding data or shrinking BEL scope over introducing
   new locks with new ordering obligations.

Cross-lock ordering violations (acquiring the BEL while holding `partition_mutex_`)
cannot be detected by the wrappers' own owner fields alone, but they are structurally
impossible if rule 2 holds — the backend only takes `partition_mutex_` in leaf
bookkeeping functions that never call upward. That property is enforceable by review of
a small, closed set of acquisition sites inside one file per backend.

## 8. KVM backend (secondary)

Until this phase lands, the KVM backend reports `supports_multiple_vcpus() == false`
and rejects `vcpu_count > 1` like the JIT backends, even though the hypervisor itself
could support it.

Mirror of §4, structurally identical:

- N `KVM_CREATE_VCPU` fds + per-vCPU `kvm_run` mmaps (`kvm_x86_64_emulator.cpp:1362-1380`).
- Per-vCPU: `vcpu_thread_`, kick signal (`pthread_kill`, `:577-594` — already the right
  per-thread cancel model), and crucially the **register caches**
  (`regs_cache_`/`sregs_cache_`, `:2231-2266`) which are per-vCPU state.
- VM-global: memslots, GPA tables, hook maps → same `partition_mutex_` treatment.
- The design deliberately keeps everything KVM-specific out of windows-emulator: the
  scheduler/BEL only relies on the interface contract from §3.

Unicorn and icicle remain `supports_multiple_vcpus() == false`; at N=1 the unified
scheduler runs a single worker with the same yield points and an uncontended BEL —
functionally equivalent to today's loop.

---

## 9. Implementation phases

Each phase builds, passes `analyzer.exe -s test-sample.exe`, and is independently
mergeable.

**Phase 0 — mechanical refactor, zero behavior change.**
Extract `x86_64_cpu` from the emulator interface (backends expose their single CPU as
index 0); add the triggering `x86_64_cpu&` parameter to all hook callback signatures;
introduce `vcpu_context` and thread it through `syscall_context`, exception dispatch and
user-callback dispatch, deleting `windows_emulator::current_thread()` and
`process_context::active_thread` (compiler-guided); add `supports_multiple_vcpus()` to
all backends; plumb `vcpu_count` (default 1) through `backend_selection` and
`emulator_settings` with the hard error for unsupported backends. Still exactly one
vCPU and one host thread. Risk: low (no behavior change, the compiler drives the
refactor). Churn: high but mechanical.

**Phase 1 — WHP backend goes N-vCPU internally.**
`whp_vcpu` struct, per-index VP creation, `ProcessorCount = N`, parameterized register/
run/cancel calls, `partition_mutex_` around partition tables and hook dispatch
containers. The windows-emulator still only uses CPU 0 → no visible change. Add a
backend-level unit test that creates N VPs, runs trivial code on two host threads in
disjoint memory, exercises concurrent map/unmap. Empirically verify
`WHvMapGpaRange`/`WHvUnmapGpaRange` while other VPs run.

**Phase 2 — the BEL + scheduler workers (the big one).**
Introduce the kernel mutex; acquire it in every emulator-entry callback installed by
`setup_hooks()` and in the UI/external entry points; replace the single run loop with
`vcpu_worker`s + `ready_cv` + timer-kick thread; per-vCPU `switch_thread_`; device pump
on scheduler ticks; `logger` mutex; atomic instruction counters. Exposed as a setting
(`--vcpus N`, default 1). N=1 goes through the same scheduler (one worker, uncontended
BEL) — benchmark N=1 boot wall-time against main before merging. Startup validation:
hard error when `vcpu_count > 1` is combined with an unsupported backend, gdb-stub,
instruction precision, instruction budgets, relative-time mode, or snapshots (until
Phase 3).

**Phase 3 — cross-vCPU correctness.**
Kicks for alert/APC/suspend/terminate targeting running threads; force-save for
`NtGet/SetContextThread`; stop-the-world helper; snapshot quiesce (lifts the N=1
snapshot restriction); guest-visible CPU identity (KUSD count,
`NtGetCurrentProcessorNumberEx`).

**Phase 4 — validation + performance.**
Guest stress test (in the test-sample or a dedicated sample: N threads hammering
interlocked ops, events, mutants, waits, VirtualAlloc churn) run at `--vcpus 2..8`;
BEL contention profiling (extend the `SOGEN_WHP_PROFILE` infrastructure); then targeted
lock refinement *only where measured*: likely candidates are KUSD reads, hot device
ioctls (GPU bridge), and `memory_manager` read paths (a `shared_mutex` split).

**Phase 5 — secondary targets.**
KVM parity (§8); linux-emulator adopts the same scheduler/BEL shape (it mirrors the
windows-emulator structure; consider extracting the vCPU worker/scheduler core into a
shared component at that point, not before).

---

## 10. Risks and open questions

- **WHP remap-while-running semantics** (§4). If `WHvUnmapGpaRange` during concurrent VP
  execution misbehaves, every unmap/protect needs stop-the-world — correct but slower;
  the `remap_pages` coalescing (`:2196`) limits the frequency. Resolve in Phase 1.
- **BEL contention.** If a workload turns out syscall-heavy, the parallel win shrinks.
  Mitigation path is §9 Phase 4; the architecture doesn't change.
- **Single-vCPU regressions.** The BEL is uncontended at N=1 (a fetch-and-store on
  acquire), but Phase 2 must benchmark boot wall-time at N=1 against main before
  merging, since N=1 runs through the new scheduler.
- **Latent single-thread assumptions.** Grep-audits won't find everything (e.g. static
  locals in syscall handlers, scratch buffers reused across calls — the gpu-bridge
  scratch buffers from #1172 are per-`windows_emulator`, fine, but the same pattern
  elsewhere may not be). Phase 4 stress testing is the real net.
- **Wait semantics under concurrency.** `is_thread_ready` / `consume_object_signal`
  (`emulator_thread.cpp:72,176,935`) stay BEL-protected, so "consume exactly one signal"
  stays atomic. But *wakeup ordering* changes: today round-robin position determines who
  wins a signaled auto-reset event; with parallel workers it's whichever worker picks the
  thread first. Windows makes no ordering promise, so this is legal — but it may surface
  latent guest bugs that were masked by deterministic scheduling. Keep `--vcpus 1` as the
  deterministic-repro fallback.
- **Determinism / snapshots.** Multi-vCPU runs are inherently non-reproducible;
  snapshot/restore stays supported (quiesced) but replay-style debugging needs N=1.
- **WOW64.** Heaven's-gate transitions (`wow64_heaven_gate.hpp`) are per-thread register
  state inside the blob — no extra work expected, but the 32-bit paths must be in the
  Phase 4 stress matrix (open-iw5, t6mp are the practical testbeds).
