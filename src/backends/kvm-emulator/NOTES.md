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

## Current blocker — ROOT CAUSE IDENTIFIED: preemption is too coarse

The multi-threaded failure is a **scheduling / thread-interleaving problem**, not a
register or context bug.

The scheduler (`windows_emulator::start`) drives thread time-slicing two ways:
- instruction-precise backends (unicorn): run exactly `MAX_INSTRUCTIONS_PER_TIME_SLICE`
  (`0x20000` = 131072) instructions per slice, then switch — **deterministic**.
- non-instruction-precise backends (WHP, KVM): a helper thread fires every 20ms wall-clock
  and calls `emu().stop()` to preempt — **non-deterministic and coarse**.

KVM runs **near-native**, so a 20ms slice is *millions* of guest instructions. A thread (e.g.
a thread-pool worker, `TppWorkerThread` at `0x180075bc0`) monopolizes the single vCPU and runs
far ahead of where the cooperative model expects, so other threads never get to publish state
the worker depends on. The worker then takes an ntdll fatal path (`STATUS_UNSUCCESSFUL` at
`0x18008c4e0`) and runs into the `int3` padding at `0x18008c50f` (the breakpoint is a
*symptom*). WHP avoids this only because it is much slower than KVM, so it executes far fewer
instructions per 20ms.

**Evidence:** shortening the interval from 20ms to 50µs took the run from **782 → ~2946
syscalls** (then plateaus, because 50µs of near-native KVM is still a lot of instructions, and
the interleaving is still non-deterministic — it later dies in `__chkstk` stack-probe at
`0x180167007` after an `NtRaiseException` storm from threads still hitting error paths).

### The proper fix
The goal is to preempt each thread after a fixed *instruction count* (≈`0x20000`, matching
the deterministic interleaving the instruction-precise backends use) instead of after a
wall-clock interval. Use the host PMU to count retired guest instructions:
`perf_event_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, exclude_host=1, pinned)`
on the vCPU thread, `sample_period = slice`, with overflow delivering a signal (`fcntl`
`F_SETOWN`/`F_SETSIG`, like the existing `SIGRTMIN` kick) so `KVM_RUN` returns `EINTR` after
~`slice` retired guest instructions. PMU skid is fine — retired-instruction counts are
deterministic for the same code, so the schedule stays reproducible.

**Important nuance — do NOT just flip `uses_instruction_precision()` on.** That path
(`windows_emulator::on_instruction_execution`) relies on a **per-instruction hook** that
increments `executed_instructions_` and yields every `MAX_INSTRUCTIONS_PER_TIME_SLICE`. KVM
cannot deliver per-instruction callbacks without single-stepping (an exit per instruction —
unusably slow), so the existing instruction-precise path does not fit KVM. Two viable
integrations instead:
- **(A) PMU-driven preemption trigger (smaller change):** keep the non-precise scheduler
  path, but replace/augment the 20ms wall-clock helper in `windows_emulator::start` so the
  preemption is driven by the PMU instruction-count overflow rather than wall-clock. The
  overflow must still set `switch_thread_` and stop the vCPU (same as today's timer), so the
  trigger has to feed back into `windows_emulator` (e.g. a backend-provided "instruction
  budget" the scheduler arms before each `start`, or a callback the backend invokes on
  overflow). Net effect: fine-grained, deterministic interleaving without per-instruction
  hooks.
- **(B) Count-based `start(count)` (larger change):** make `start(count)` run ≈`count`
  instructions via the PMU and have the backend report the actual retired count back so the
  scheduler can advance `executed_instructions_` from it (rather than from the per-instruction
  hook). This needs a scheduler path that updates `executed_instructions_` from a
  backend-reported delta when `supports_instruction_counting()` is true but per-instruction
  hooks are unavailable.

(A) is the recommended first step. The current `SIGRTMIN`-based `stop()` kick already proves
the vCPU can be interrupted out of `KVM_RUN`; the PMU just changes *when* that fires from
wall-clock to instruction-count.

### Things ruled out along the way
- It is **not** a register/context-restore bug: the crashing worker's `NtContinue` context is
  byte-identical to unicorn's (`Rip=0x18008c510`, correct start params), and thread switch uses
  the backend's `save_registers`/`restore_registers` (full kvm_regs+sregs+fpu+MSRs — complete),
  not `cpu_context`.
- **FP/XMM/MxCsr** save-restore: disabling `CONTEXT_XSTATE` in `cpu_context.cpp` did not change
  the failure.
- **Wall-clock time**: running with `-rep` (reproducible time) did not change it; not a KUSD
  time-refresh issue.
- Host/nested-virt, CPL3, CPUID, CR0/CR4/XCR0 were ruled out for the earlier MMIO `#UD`.

### Diagnostic helpers used (re-add if continuing)
- Per-syscall register dump in the backend-agnostic `syscall_dispatcher::dispatch` (gated on a
  `SCDUMP` env var), diffed KVM vs unicorn. Caveat: `rdi`/`rbx` hold security cookies (random
  run-to-run) — ignore them; use `rsp`/`rbp`/`rsi`/`r12`–`r15`/`gs` base. Group dumps by `gs`
  base to separate threads.
- `NtContinue` context dump in `handle_NtContinueEx`.

Candidate areas still open: x87/FP state (not just XMM), EFLAGS handling, segment
selector/base restore on context switch, debug registers, and the thread save/restore path
(`emulator_thread::save`/`restore` → `cpu_context`).

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
