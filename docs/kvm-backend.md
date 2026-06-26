# KVM backend — development notes

The Linux KVM emulator backend (`sogen::kvm`). A standalone backend that mirrors
`whp-emulator` (the WHP backend is left untouched), selected at runtime via
`EMULATOR_KVM=1` on Linux x86_64.

## Status

`test-sample.exe` runs to completion under KVM with all 26 tests passing (exit 0) on both
Intel and AMD hosts. Single-threaded and multi-threaded execution, paging, syscalls
(LSTAR→HLT), exception delivery, and MMIO all work.

## Architecture

- `kvm_x86_64_emulator.hpp` — factory `sogen::kvm::create_x86_64_emulator()`.
- `kvm_x86_64_common.hpp` — vendored helpers (page-table walk, MMIO region lookup,
  GP-register classification, `access_memory`); only what KVM uses.
- `kvm_x86_64_emulator.cpp` — the backend.

Guest model: 64-bit long mode, runs the Windows user process at CPL3. The backend builds
the guest page tables itself (`pml4`, `ensure_virtual_mapping`), maps guest physical pages
into KVM via `KVM_SET_USER_MEMORY_REGION` memslots (`rebuild_mappings`, which coalesces
contiguous same-permission pages), and intercepts:

- **syscalls** via `MSR_LSTAR` pointing at an internal page whose first byte is `HLT`; the
  run loop recognizes the HLT at `syscall_hook_page_+1` and calls `handle_syscall_halt`.
- **exceptions** via a synthetic IDT (see fix 2 below).
- **cpuid / rdtsc / rdtscp / ud2** via `handle_pre_run_instruction`, which scans the bytes
  at RIP before each `KVM_RUN`.

CMake gates the backend to `Linux AND NOT ANDROID AND x86_64`; `backend-selection` adds
`backend_type::kvm` and the `EMULATOR_KVM=1` env var.

The fine-grained `hook_memory_read/write/execution/range_execution` and `hook_basic_block`
hooks are registered for API compatibility but never fire: guest code runs natively, so
there is no per-access/per-instruction instrumentation point short of single-stepping.
Analyzer features that depend on them are unsupported under this backend.

## Fixes that made it work

1. **Preemption / hang.** The scheduler preempts a running thread every 20ms via
   `emu().stop()` from a helper thread. The original `stop()` only set
   `run_->immediate_exit = 1`, which was never reset (so after the first preemption every
   `KVM_RUN` returned instantly) and is only checked at `KVM_RUN` entry (so it can't
   interrupt a vCPU already in guest mode). Fix: reset `immediate_exit` each slice and
   `pthread_kill` the vCPU thread with a no-op `SIGRTMIN` (installed without `SA_RESTART`)
   so an in-flight `KVM_RUN` returns `EINTR`.

2. **Exception interception.** KVM exposes no userspace exception-exit bitmap (unlike WHP).
   Install an internal IDT whose 64-bit interrupt gates point at one-`HLT` stubs running at
   CPL0 on an IST stack (via a TSS set through `KVM_SET_SREGS`). On the HLT exit the run
   loop derives the vector from the trapping RIP, reconstructs the faulting frame from the
   IST stack, restores the faulting context, and routes to `handle_exception`.

3. **MMIO / SSE.** KVM services MMIO by emulating the faulting instruction, and its
   in-kernel emulator lacks SSE/AVX (ntdll runs a vectorized `wcslen` over
   `KUSER_SHARED_DATA`). Fix: back MMIO pages with a **read-only memslot** seeded from the
   read callback and refreshed before each guest entry (`refresh_mmio_pages`), so reads
   execute natively and only writes trap.

4. **Syscall-return RIP advance.** `handle_syscall_halt` only advanced RIP past the syscall
   when the handler left RIP unchanged, but `dispatch_callback`'s instrumentation-callback
   redirect sets RIP to `callback - 2` expecting the syscall length added back. Always
   advance by 2 in the non-skip branch (matching WHP), otherwise a redirected thread
   resumed two bytes early.

5. **AMD: pending exception re-injection.** After the synthetic IDT delivers an exception
   and we rewind to the faulting context, KVM still holds a pending exception in
   `kvm_vcpu_events`. On AMD (SVM) it is re-injected on the next entry → triple fault
   (`KVM_EXIT_SHUTDOWN`); Intel tolerates it. Clear `kvm_vcpu_events` after handling an
   exception. This is the KVM analog of the WHP backend's AMD fix (PR #808).

6. **Breakpoint RIP off-by-one.** int3 is a trap, so the CPU pushes `int3 + 1`. The
   emulator's exception dispatch expects RIP at the int3 (for `ExceptionAddress` and the
   int-2Dh debug-service pattern), as the instruction-precise backends report. Rewind RIP
   by one for `#BP` in the trap handler.

Fixes 5 and 6 only reproduce with the Windows 2022 emulation root (a different ntdll than
some local roots), and fix 5 only on AMD.

## Build & test (Docker, from a Windows host)

KVM cannot build or run on Windows. Use Docker Desktop (WSL2 backend); nested virtualization
is on by default, so `/dev/kvm` is available with `--device /dev/kvm`.

Build image:

```dockerfile
FROM gcc:14
RUN apt-get update -qq && apt-get install -y -qq cmake ninja-build linux-libc-dev >/dev/null
```

Configure (separate build dir `build/linux`), disabling Rust/Python and using vendored SDL
in console mode:

```sh
docker run --rm -v "<repo>:/src" -w /src sogen-kvm-build \
  cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSOGEN_ENABLE_RUST_CODE=OFF -DSOGEN_ENABLE_PYTHON_BINDINGS=OFF \
    -DSOGEN_ENABLE_SDL3=ON -DSOGEN_USE_SYSTEM_SDL3=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON \
    -DSOGEN_ENABLE_LTO=OFF -DSOGEN_BUILD_TOOLS=ON
```

Build and run (`kvm-emulator` is a shared lib the analyzer loads, so it can be rebuilt
alone for fast iteration):

```sh
docker run --rm -v "<repo>:/src" -w /src sogen-kvm-build \
  cmake --build build/linux --target analyzer -j

docker run --rm --device /dev/kvm -v "<repo>:/src" -w /src/build/linux/artifacts \
  sogen-kvm-build \
  env EMULATOR_KVM=1 ./analyzer -s -e /src/build/linux/artifacts/root c:/test-sample.exe
```

Notes:
- A Windows emulation root is at `build/linux/artifacts/root` (generated on Windows; CI
  generates it and the Linux jobs download it). `-s` is silent mode; drop it for the
  verbose syscall log.
- `clang-format` is not in the base image; run it on the host instead.
- The unicorn backend (omit `EMULATOR_KVM`) runs the same sample to completion and is the
  reference for trace diffing.
- AMD-specific issues may need an AMD host (a GitHub AMD runner / a physical AMD box);
  the Docker-built Linux binaries also run directly on a bare-metal Linux box.

### Windows-path gotcha for Docker on Git Bash

Bind-mount paths must be Windows-style; prefix with `MSYS_NO_PATHCONV=1` so Git Bash does
not mangle them, e.g. `MSYS_NO_PATHCONV=1 docker run ... -v "C:\path\to\repo:/src" ...`.
