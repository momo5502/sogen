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

## Current blocker (unfixed)

A **deterministic, zero-page-fault, multi-threaded context/state divergence**.

- Deterministic: 782 syscalls, same failure every run.
- **Zero page faults** the entire run — so it is a pure data/state divergence, not a fault.
- The syscall trace matches the unicorn backend **exactly for 773 syscalls** (same names and
  caller addresses), then the **thread interleaving** diverges. This is partly expected: KVM
  preempts on a 20ms wall-clock while unicorn time-slices by instruction count, so the
  interleaving differs. But WHP also preempts on wall-clock and runs the sample fine, so
  there is a real KVM-specific bug exposed by the multi-threaded schedule, not just the
  interleaving.
- A thread then enters an ntdll fatal path (`STATUS_UNSUCCESSFUL` at ntdll `0x18008c4e0`,
  which reads `gs:[0x20]` then `[rax+0xa8]`), calls a no-return fail-fast, and runs into the
  `int3` alignment padding at `0x18008c50f` → `NtRaiseException` → "Emulation terminated
  without status." So the `int3` is a **symptom**; the breakpoint dispatch itself is fine.
- It happens right after an **APC dispatch → `NtContinue`** cycle, which does a full
  `cpu_context::save`/`restore` of all registers.

### Ruled out
- **FP/XMM/MxCsr save-restore**: disabling the `CONTEXT_XSTATE` block in
  `src/windows-emulator/cpu_context.cpp` (both save and restore) did **not** change the
  failure. So XMM/MxCsr is not the cause.
- Host / nested-virt limitation, CPL3, CPUID (XSAVE/SSE bits), and CR0/CR4/XCR0 static state
  were all ruled out for the earlier MMIO `#UD` via standalone KVM test programs; SSE works
  fine in a minimal long-mode CPL3 KVM guest on this host.

### Diagnostic approach for next time
Dump callee-saved registers per syscall in the **backend-agnostic** dispatcher
(`syscall_dispatcher::dispatch`) for both KVM and unicorn, then diff the aligned traces to
find the first register that diverges (the first 773 syscalls align 1:1). Caveat: some
registers hold **security cookies / high-entropy random values** (observed in `rdi` and
`rbx` at the very first syscalls) that differ run-to-run and between backends; ignore those
and focus on deterministic registers (`rsp`, `rbp`, `rsi`, `r12`–`r15`, `gs` base). The
divergence in a *deterministic* register pinpoints the bug.

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
