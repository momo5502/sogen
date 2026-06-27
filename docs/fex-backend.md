# FEX backend — development notes

The FEX-Emu emulator backend (`sogen::fex`). A standalone backend that mirrors the structure of the
other backends, selected at runtime via `EMULATOR_FEX=1`. It targets **AArch64 Linux/Android hosts**
— [FEX](https://fex-emu.com) only JITs x86/x86-64 to ARM64 — and is aimed primarily at improving
Android support (see issue #1045).

## Status

> **Experimental scaffold.** This is a first-pass implementation that establishes the structure,
> register/memory model, and syscall bridge. It is **not yet known to build or run**: it depends on
> an external FEXCore that is not vendored, only compiles for ARM64, and several integration points
> with FEX internals are marked `TODO(fex)` in the source. It is intentionally merged as a starting
> point to iterate on, not as a working backend.

What is in place:

- Full `x86_64_emulator` interface implementation (registers, memory, hooks, serialize).
- `x86_register` ↔ `FEXCore::Core::CPUState` mapping (`fex_x86_64_common.hpp`).
- A `FEXCore::HLE::SyscallHandler` that routes guest `syscall` instructions to the registered
  syscall instruction-hook (the path the Windows emulation layer uses to service NT syscalls).
- Build, backend-selection, and Python-binding wiring behind `SOGEN_ENABLE_FEX`.

What still needs work (the `TODO(fex)` markers):

- FEXCore config/static-table initialization and `HostFeatures` fetch in `initialize_context()`.
- A `SignalDelegator` for guest fault/signal delivery (required by FEX for exceptions).
- Confirming the exact `InternalThreadState` → `CPUState` access path for the pinned FEX version.
- Making an in-flight `ExecuteThread()` return on `stop()` (cooperative cancellation).
- Snapshot of mapped memory in `serialize_state` (currently registers only).

## Architecture

- `fex_x86_64_emulator.hpp` — factory `sogen::fex::create_x86_64_emulator()`.
- `fex_x86_64_common.hpp` — header-only `x86_register` → `CPUState` field mapping (GPRs and their
  sub-registers, rip, flags, xmm, mm, fs/gs base, segment selectors, mxcsr/fcw). No FEX includes, so
  it can be reasoned about without the FEX toolchain.
- `fex_x86_64_emulator.cpp` — the backend and the syscall bridge.

### Guest model — the key difference from the other backends

FEX is an **in-process** binary translator. It does not sandbox a separate guest address space:
translated guest code runs inside the host process and **guest virtual addresses are host virtual
addresses** (a 1:1 mapping). This drives the whole design:

- `map_memory()` is a real `mmap(MAP_FIXED)` at the guest address; `unmap_memory()`/
  `apply_memory_protection()` are `munmap`/`mprotect`. A sorted region map tracks what is mapped.
- `read_memory()`/`write_memory()` are direct host `memcpy`s once the range is confirmed mapped (the
  guest pointer is directly dereferenceable). Writes invalidate FEX's translation cache for the range.
- `map_host_memory()` aliases caller-owned memory into the guest with `mremap(MREMAP_FIXED)` (e.g. for
  the GPU bridge), and is *not* munmap'd on teardown.

This makes FEX a close cousin of the KVM backend in *capability*: the guest runs natively, so there
is **no per-access or per-instruction instrumentation point**. The fine-grained
`hook_memory_read/write/execution/range_execution` and `hook_basic_block` hooks are registered for API
compatibility but never fire, and `supports_global_memory_execution_hooks()` returns `false`. Only the
`syscall` instruction hook is wired (via the syscall handler). Analyzer features that depend on the
unfired hooks are unsupported under this backend, exactly as with KVM.

### Syscall interception

FEX delivers guest `syscall` instructions to a `FEXCore::HLE::SyscallHandler::HandleSyscall`. The
backend's `fex_syscall_handler` forwards these to the registered syscall instruction-hook. The hook
(the Windows emulation layer) reads and writes guest registers itself through the emulator and places
the NT status in `RAX`; the handler returns that value so FEX preserves it.

## Build

The backend is off by default and gated to ARM64 Linux/Android:

```sh
cmake --preset=release \
  -DSOGEN_ENABLE_FEX=ON \
  -DSOGEN_FEX_ROOT=/path/to/fex/install   # or make FEXCore discoverable via find_package(FEXCore)
```

CMake (`src/backends/CMakeLists.txt`) only adds the subdirectory when `SOGEN_ENABLE_FEX` is set and
the host is `aarch64`/`arm64`. `backend-selection` links `fex-emulator`, defines `SOGEN_ENABLE_FEX`,
adds `backend_type::fex`, and honors `EMULATOR_FEX=1`. The Python `Backend` enum gains `fex`.

FEXCore itself is **not** vendored as a submodule (it is large and ARM64-host-specific); the build
locates it via `find_package(FEXCore)` or `SOGEN_FEX_ROOT`.

## Selecting the backend

```sh
EMULATOR_FEX=1 ./analyzer -e root c:/test-sample.exe
```

or, from Python, `sogen.Backend.fex`.
