# KVM backend — development notes

Status of the Linux KVM emulator backend (`sogen::kvm`) on the `kvm-backend` branch.
This is a work-in-progress backend, ported from the abandoned `hypervisor` branch and
rebuilt as a standalone backend that mirrors `whp-emulator` (the WHP backend is left
untouched).

## Current state

`analyzer` running `test-sample.exe` under KVM gets from "hangs immediately" to
**~782 syscalls deep** — through all of ntdll initialization and the parallel loader,
into kernelbase and application code. Single-threaded CPU emulation, paging, syscalls
(LSTAR→HLT), exception delivery, and MMIO all work.

It then fails on a **multi-threaded context/state divergence** (see "Current blocker").

## Architecture

- `kvm_x86_64_emulator.hpp` — factory `sogen::kvm::create_x86_64_emulator()`.
- `kvm_x86_64_common.hpp` — vendored helpers (page-table walk, MMIO region lookup,
  GP-register classification, `access_memory`). Subset of the old shared hypervisor
  header; only what KVM uses.
- `kvm_x86_64_emulator.cpp` — the backend.

Guest model: 64-bit long mode, runs the Windows user process at CPL3. The backend builds
the guest page tables itself (`pml4`, `ensure_virtual_mapping`), maps guest physical pages
into KVM via `KVM_SET_USER_MEMORY_REGION` memslots (`rebuild_mappings`, which coalesces
contiguous same-permission pages), and intercepts:
- **syscalls** via `MSR_LSTAR` pointing at an internal page whose first byte is `HLT`;
  the run loop recognizes the HLT at `syscall_hook_page_+1` and calls `handle_syscall_halt`.
- **exceptions** via a synthetic IDT (see fix 2 below).
- **cpuid / rdtsc / rdtscp / ud2** via `handle_pre_run_instruction`, which scans the bytes
  at RIP before each `KVM_RUN`.

CMake gates the backend to `Linux AND NOT ANDROID AND x86_64`; `backend-selection` adds
`backend_type::kvm` and the `EMULATOR_KVM=1` env var.

## Fixes landed (committed + pushed)

1. **Preemption / hang** (`197c1a20`). The scheduler preempts a running thread every 20ms
   by calling `emu().stop()` from a helper thread. The original `stop()` only set
   `run_->immediate_exit = 1`, which (a) was never reset, so after the first preemption every
   `KVM_RUN` returned instantly and the guest froze, and (b) is only checked at `KVM_RUN`
   entry, so it cannot interrupt a vCPU already in guest mode. Fix: reset `immediate_exit`
   each run slice and additionally `pthread_kill` the vCPU thread with a no-op `SIGRTMIN`
   (installed without `SA_RESTART`) so an in-flight `KVM_RUN` returns `EINTR`. WHP uses
   `WHvCancelRunVirtualProcessor` for the same purpose.

2. **Exception interception** (`4d19271d`). KVM exposes no userspace exception-exit bitmap
   (unlike WHP's `ExceptionExit`). Guest faults must be trapped through guest structures.
   Install an internal IDT whose 64-bit interrupt gates point at one-`HLT` stubs running at
   CPL0 (reusing the ring-0 code selector `0x08` the emulator already puts in its GDT),
   backed by a TSS with an IST stack set via `KVM_SET_SREGS`. On the HLT exit the run loop
   derives the vector from the trapping RIP, reconstructs the faulting frame from the IST
   stack, restores the faulting context, and routes to `handle_exception` (which now also
   takes the `#PF` error code). This unblocks the emulator's memory-violation hook
   (stale-gs-base fixup, guard-page stack growth, SEH dispatch). Without it, the first guest
   fault triple-faulted into `KVM_EXIT_SHUTDOWN`.

3. **MMIO / SSE** (`808ae20a`). The backend relied on `KVM_EXIT_MMIO` for MMIO regions
   (mapped with no memslot). KVM services such accesses by **emulating** the faulting
   instruction, and its in-kernel emulator does not support SSE/AVX. ntdll runs a vectorized
   `wcslen` (`pcmpeqw`) over the `NtSystemRoot` string in KUSER_SHARED_DATA (an MMIO region),
   so the access trapped and KVM injected `#UD`; the emulator's illegal-instruction handler
   then skipped into the middle of the instruction, corrupting execution and aborting init
   with `STATUS_APP_INIT_FAILURE`. Fix: back MMIO pages with a **read-only memslot** seeded
   from the read callback and refreshed before each guest entry (`refresh_mmio_pages`), so
   reads (incl. SSE/AVX) execute natively; only writes trap and go to the write callback.

## Current blocker — a thread-readiness / timing sensitivity (NOT preemption coarseness)

The multi-threaded failure is a **scheduling / thread-interleaving problem**, not a register
or context bug. A thread-pool worker (`TppWorkerThread` at `0x180075bc0`) takes an ntdll
fatal path (`STATUS_UNSUCCESSFUL` at `0x18008c4e0`) and runs into the `int3` padding at
`0x18008c50f` (the breakpoint is a *symptom*; the breakpoint dispatch itself is fine), ending
in an `NtRaiseException` and a premature stop at ~782 syscalls.

### What was tried and what it ruled out
An earlier hypothesis was "20ms wall-clock preemption is too coarse for near-native KVM."
That was **implemented and disproven**:
- A full **PMU-driven instruction-count preemption** was built (and works): `perf_event_open`
  `PERF_COUNT_HW_INSTRUCTIONS` with `exclude_host`, overflow signal via `fcntl`
  `F_SETSIG`/`F_SETOWN_EX` so `KVM_RUN` returns `EINTR` after a fixed number of retired guest
  instructions, plus `cpu_interface::self_preempts()`/`take_pending_reschedule()` and a small
  `windows_emulator::start` hook to skip the wall-clock helper and reschedule on overflow. The
  PMU counter is exact (standalone test: 2001 for ~2002 expected) and the overflow signal
  fires and reschedules correctly. **Requires `--privileged`/`CAP_PERFMON`.**
- Result: with the PMU slice at `0x20000`, `0x8000`, `0x2000`, `0x800`, and even `0x40` (64
  instructions!), the run still fails at **~780 syscalls** — *no change*. So finer
  instruction-count preemption does **not** fix it.
- Yet shortening the **wall-clock** interrupt from 20ms to 50µs *did* move the run to **~2946
  syscalls** (before dying later in a `__chkstk` stack probe at `0x180167007`).

**Conclusion:** during syscall-heavy init the main thread is usually the only *runnable*
thread (the pool workers are blocked waiting for work), so preempting it — at any granularity
— just resumes it; preemption granularity is irrelevant there. The 50µs wall-clock helped only
as an **incidental timing side-effect** (far more frequent scheduler iterations change when
time-based waits resolve and when workers become runnable), not via finer preemption. The real
blocker is a **thread-readiness / timing sensitivity** in the cooperative scheduler: a specific
ordering of when workers become runnable vs. when the main thread publishes the state they
read. unicorn's deterministic instruction-precise per-thread scheduling happens to produce an
ordering that works; KVM's does not. So the PMU work was reverted (correct mechanism, needs
privilege, doesn't fix the goal).

### Where to look next
- Instrument **thread readiness**: when does each pool worker (`gs` base identifies the thread)
  become runnable relative to the main thread's worker-factory setup syscalls
  (`NtSetInformationWorkerFactory`, `NtSetEvent`, `NtAssociateWaitCompletionPacket`)? Compare
  KVM vs unicorn ordering of *which thread runs each syscall*, not just the syscall sequence.
- The worker reaches `0x18008c4e0` (a fatal stub) from inside `TppWorkerThread`; find which
  thread-pool field/work-item it reads that is not yet published — likely a TP_POOL / work-queue
  field the main thread initializes slightly later under KVM's ordering.
- Consider whether the emulator's wait/event/worker-factory syscall handlers have an
  ordering assumption that only the instruction-precise interleaving satisfies (a latent race
  exposed by any other valid interleaving, KVM or WHP-fast).

### Things ruled out along the way
- The crashing thread is **tid 16, the loader worker** (`TppWorkerThread` at `0x180075bc0`,
  param `0x103901d30`). At its `NtContinue` start, **every input is byte-identical to unicorn**:
  registers/`Rip=0x18008c510`, the param context memory (`0x103901d30`, first 0x100 bytes), the
  global it branches on (`[0x1801d3730]=0x103c1e8c0`), and `TEB.FiberData=0`. It still diverges
  *during* execution → it must read **shared state that other threads mutate while it is yielded**
  (it yields at `NtContinue` with `TEST_ALERT`, other threads run, then it resumes and crashes).
  This is the interleaving/ordering sensitivity, not a bad input.
- **Not** a register/context-restore bug: thread switch uses the backend's
  `save_registers`/`restore_registers` (full kvm_regs+sregs+xsave+MSRs), not `cpu_context`.
- **FP/XMM/MxCsr** save-restore: disabling `CONTEXT_XSTATE` in `cpu_context.cpp` did not change it.
- **AVX/YMM vector state**: switching thread-switch save/restore from `KVM_GET_FPU` (legacy,
  XMM-only) to `KVM_GET_XSAVE` (full YMM) — a real correctness improvement, kept — did **not**
  change the failure.
- **CPUID feature set**: masking AVX/AVX2 out of the guest CPUID (to match the emulator's
  documented SSE-only set) did **not** change it. (Left CPUID at host features; the
  AVX-vs-emulator-intent question is open but not the cause here.)
- **Processor count**: forcing 1 CPU did **not** disable the parallel loader (tid 16 is still
  created) and did **not** change the failure.
- **Wall-clock time**: `-rep` (reproducible time) did not change it.
- Host/nested-virt, CPL3, CR0/CR4/XCR0 were ruled out earlier for the MMIO `#UD`.

### Diagnostic helpers used (re-add if continuing)
- Per-syscall register dump in the backend-agnostic `syscall_dispatcher::dispatch` (gated on a
  `SCDUMP` env var), diffed KVM vs unicorn. Caveat: `rdi`/`rbx` hold security cookies (random
  run-to-run) — ignore them; use `rsp`/`rbp`/`rsi`/`r12`–`r15`/`gs` base. Group dumps by `gs`
  base to separate threads.
- `NtContinue` context dump in `handle_NtContinueEx`.
- Group per-syscall dumps by `gs` base to see which thread runs each syscall; the divergence
  is in thread *ordering/readiness*, so compare the (thread, syscall) sequence between KVM and
  unicorn, not just the syscall sequence.

## Build & test (Docker, from a Windows host)

KVM cannot build or run on Windows. We use Docker Desktop (WSL2 backend) — nested
virtualization is on by default, so `/dev/kvm` is available inside a container with
`--device /dev/kvm` (run as root, the default; the device node is `root:kvm crw-rw----`).

### Reusable build image

```dockerfile
FROM gcc:14
RUN apt-get update -qq && apt-get install -y -qq cmake ninja-build linux-libc-dev >/dev/null
```

```sh
docker build -t sogen-kvm-build -f sogen-kvm.Dockerfile <context>
```

### Configure (out-of-source build dir `build/linux`, separate from the Windows `build/`)

Disable Rust/Python and use the vendored SDL in console mode (so configure passes without
X11/Wayland; the host UI backend isn't needed for analyzer runs):

```sh
docker run --rm -v "<repo>:/src" -w /src sogen-kvm-build \
  cmake -S . -B build/linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSOGEN_ENABLE_RUST_CODE=OFF \
    -DSOGEN_ENABLE_PYTHON_BINDINGS=OFF \
    -DSOGEN_ENABLE_SDL3=ON \
    -DSOGEN_USE_SYSTEM_SDL3=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
    -DSOGEN_ENABLE_LTO=OFF \
    -DSOGEN_BUILD_TOOLS=ON
```

### Build

```sh
# Just the backend (fast iteration; it's a shared lib the analyzer loads):
docker run --rm -v "<repo>:/src" -w /src sogen-kvm-build \
  cmake --build build/linux --target kvm-emulator -j

# The analyzer (needed to run a sample):
docker run --rm -v "<repo>:/src" -w /src sogen-kvm-build \
  cmake --build build/linux --target analyzer -j
```

### Run a sample under KVM

A Windows emulation root is at `build/linux/artifacts/root` (generated on Windows via
`src/tools/create-root.bat`; CI generates it and the Linux jobs download it). The sample is
loaded as a guest path under `root/filesys/c/`.

```sh
docker run --rm --device /dev/kvm -v "<repo>:/src" -w /src/build/linux/artifacts \
  sogen-kvm-build \
  env EMULATOR_KVM=1 ./analyzer -e /src/build/linux/artifacts/root c:/test-sample.exe
```

Notes:
- `-s` is **silent** mode (not a smoke test). Run without it to see the verbose syscall log.
- `clang-format` is not in the base image; `apt-get install -y clang-format` ad hoc when needed.
- The unicorn backend (omit `EMULATOR_KVM`) runs the same sample to completion (exit 0, all
  24 tests pass) and is the reference for trace diffing.

### Windows-path gotcha for Docker on Git Bash

Bind-mount/`-f` paths must be Windows-style; prefix the command with `MSYS_NO_PATHCONV=1`
to stop Git Bash from mangling them, e.g.
`MSYS_NO_PATHCONV=1 docker run ... -v "C:\path\to\repo:/src" ...`.
