# FEX backend — development notes

The FEX-Emu emulator backend (`sogen::fex`). A standalone backend that mirrors the structure of the
other backends, selected at runtime via `EMULATOR_FEX=1`. It targets **AArch64 Linux and macOS hosts**
— [FEX](https://fex-emu.com) only JITs x86/x86-64 to ARM64.

## Status

**Working.** The backend builds, links, and runs on both ARM64 Linux and macOS/Apple Silicon. It
passes the full regression suite (`test-sample.exe`, `hello.exe`, `busybox.exe`) matching the
Unicorn backend's behavior byte-for-byte, and has been validated against ~40 pre-existing sample
executables (process/thread introspection, synchronization primitives, sections, security tokens,
the registry, pipes, sockets, timers, file/directory I/O, GUI dialogs, audio/multimedia, DXGK
harnesses, and 32-bit WoW64 binaries) with every remaining discrepancy traced to a backend-agnostic
gap (an unimplemented syscall, or a missing staged system-DLL export) rather than a difference
between this backend and the existing one.

**32-bit WoW64 processes are supported** (see "WoW64 support" below) and have been exercised against
a real, unmodified 32-bit commercial game (a Call-of-Duty-engine title) reaching its main menu and
sustaining real-time rendering. GPU paravirtualization has been independently confirmed working
end-to-end (guest Vulkan calls → the `\\.\SogenGpu` IOCTL bridge → the host Vulkan driver) via a
dedicated minimal test, separately from the game's own D3D9-over-DXVK rendering path.

### Performance

On a pure compute-bound workload (a tight ALU loop with no syscalls beyond timestamping), this
backend measured roughly **129x less wall-clock time** than the Unicorn backend on the same host
(~3.4s vs. ~439s for 2 billion loop iterations) — JIT-compiled native ARM64 code running directly on
the CPU, versus per-block interpretation. A mixed, syscall-heavy smoke-test workload showed a
smaller but still substantial ~27-30x gap. Expect the larger multiplier to dominate for CPU-bound
guest workloads (game logic, physics, scripting) and the smaller one where I/O/syscall dispatch is a
significant fraction of runtime.

### Known limitations

- **`comctl32.dll`-linked apps** (e.g. a bundled calculator sample) fail identically on both this
  backend and Unicorn due to a staged `comctl32.dll` build that doesn't export the ordinals the
  sample binaries were linked against. This is a staged-asset version mismatch, not a code bug, and
  is not fixable without staging a different `comctl32.dll`.
- **Real `wow64cpu.dll` dispatch is not yet exercised.** This backend intercepts and marshals state
  directly at the x86 bitness-switch ("heaven's gate") trampoline rather than executing real
  `wow64cpu.dll` code through it; the real dispatch-table-driven convention is decoded (see
  "WoW64 support" below) but not yet wired up as an alternate/fallback path.

## What is in place

- Full `x86_64_emulator` interface implementation (registers, memory, hooks, serialize).
- `x86_register` ↔ `FEXCore::Core::CPUState` mapping (`fex_x86_64_common.hpp`).
- A `FEXCore::HLE::SyscallHandler` that routes guest `syscall` instructions to the registered
  syscall instruction-hook (the path the Windows emulation layer uses to service NT syscalls).
- Real `FEXCore::HostFeatures` detection (`sysctlbyname` on macOS, MIDR-register reads on Linux).
- Fault delivery via plain POSIX `sigaction` on macOS (no Mach exception port needed): a per-guest-
  page permission shadow table reconciling Apple Silicon's 16KB host page size against the guest's
  4KB architectural page size, JIT guard-page write-protect race handling, MMIO decode-and-emulate,
  misaligned load-acquire/store-release decode-and-emulate, synthetic-`#GP`-to-real-vector
  remapping, and cooperative thread-stop via FEXCore's `InterruptFaultPage` mechanism.
- Real JIT code-buffer overflow guard pages on macOS. Apple Silicon's `MAP_JIT` memory cannot have
  its protection changed by an ordinary `mprotect()` after the fact (confirmed empirically: it always
  fails `EACCES`, regardless of the per-thread W^X toggle state), so FEXCore's own end-of-buffer guard
  page silently never took effect there. The sogen-side FEXCore-internal allocator arena now places
  the real guard by construction instead: it makes only the buffer's leading portion (excluding one
  trailing host page) the actual executable `MAP_JIT` mapping via the same real-`munmap`-then-hint-
  `mmap` placement it already uses for `MAP_JIT` allocations, and simply never touches that trailing
  page — which stays part of the arena's permanent `PROT_NONE` reservation and genuinely faults on any
  access. This applies to both the main JIT code buffer and the per-compile temporary staging buffer.
- GPU-bridge host-memory coherency (`mach_vm_remap` aliasing on macOS, `sys_dcache_flush`/`dc civac`
  cache maintenance) for the paravirtualized-GPU memory-sharing path.
- 32-bit WoW64 process support (a second, lazily-created 32-bit FEXCore Context/Thread per process,
  gate-crossing state marshaling, a WoW64 guest-memory rebase — see below).
- Build, backend-selection, and Python-binding wiring (auto-enabled on ARM64 Linux/macOS + Clang);
  FEXCore built via ExternalProject and linked.

## WoW64 support

FEXCore is fixed-bitness per `Context` — a single `Context` cannot execute both 64-bit and 32-bit
code. A WoW64 process genuinely starts execution in real 64-bit ntdll code (the thread-init thunk),
crosses into 32-bit code at the x86 "heaven's gate" bitness switch, and crosses back on every syscall
return and kernel callback. This backend models that with **two FEXCore Contexts per WoW64 process**
— a 64-bit one (handling the real 64-bit ntdll/wow64*.dll code) and a lazily-created 32-bit one
(handling the guest's own 32-bit image and its 32-bit ntdll) — switching which one is "active" at
each gate crossing.

### Gate-crossing interception

The bitness-switch trampoline is intercepted via the same generic non-executable-range mechanism
FEXCore already uses for synthetic page faults: the trampoline's guest address range is marked
non-executable, so reaching it raises a controlled synthetic `#PF` before any of its bytes are ever
JIT-compiled, which the backend catches and handles by marshaling state between the two Contexts
directly (rather than letting either Context attempt to decode/execute the other bitness's code).

State marshaling across a crossing needed to be more careful than a naive whole-`CPUState` copy:
each Context's `CPUState` also carries FEXCore-internal JIT bookkeeping (SRA-mapped GPRs, the call-ret
shadow-stack pointer, the JIT lookup-cache pointer) interleaved with genuinely-architectural x86 state
(GPRs, XMM, x87, EFLAGS, segment selectors). A correct crossing must copy the architectural state and
leave each Context's own JIT bookkeeping alone. Getting this right was the single largest source of
bugs during bring-up — a sequence of crossing-specific corruption bugs were found and fixed, each
clobbering a different piece of state across the boundary (the destination Context's GS segment base,
the reserved registers `wow64cpu.dll`'s real dispatch convention uses for its CPU-area block and
64-bit stack pointer, and the source of R8–R15 when the wrong engine's already-clobbered
static-register-allocation slots were read instead of the frozen engine's own).

### 32-bit guest memory rebase

Apple Silicon enforces a mandatory, unshrinkable 4GB `__PAGEZERO` for every 64-bit process, making
the entire low 4GB of host address space permanently unmappable. This conflicts directly with this
project's guest-VA-equals-host-VA memory model for a 32-bit guest, whose whole architectural address
space lives in that exact range. The fix is a FEXCore-side, per-Context, runtime-conditional address
rebase (a new `CONFIG_WOW64GUESTREBASE` option and `Context::SetNeedsWow64GuestRebase` API in the
`deps/FEX` submodule): when enabled on a Context, every real memory access — data or instruction
fetch — computed at an address below 4GB is transparently rebased to a fixed offset above it, via a
runtime IR `Select` rather than a compile-time-constant add (since a 64-bit-mode Context's addresses
aren't confined to a fixed range the way a 32-bit-mode Context's are). This is a strict no-op for any
Context that doesn't opt in, so it does not affect the existing 64-bit-only Linux/macOS path.

### `wow64cpu.dll`'s real dispatch convention (decoded, not yet used)

The real `TurboDispatchJumpAddressStart` mechanism inside `wow64cpu.dll` — the genuine syscall-return
dispatch table a real WoW64 process uses — was disassembled and its calling convention (an index
derived from the high word of `EAX`, dispatched through a jump table `wow64cpu.dll` builds at init,
against a CPU-area `CONTEXT` block holding the 32-bit register file) is documented in the plan history
for this effort, but this backend does not yet execute real `wow64cpu.dll` code through it — it
intercepts and marshals state directly at the bitness-switch trampoline instead, which is sufficient
for everything validated so far but means the real dispatch table is presently unused.

## Architecture

- `fex_x86_64_emulator.hpp` — factory `sogen::fex::create_x86_64_emulator()`.
- `fex_x86_64_common.hpp` — header-only `x86_register` → `CPUState` field mapping (GPRs and their
  sub-registers, rip, flags, xmm, mm, fs/gs base, segment selectors, mxcsr/fcw). No FEX includes, so
  it can be reasoned about without the FEX toolchain.
- `fex_x86_64_marshal.hpp` — the architectural-state marshaling helper used at WoW64 gate crossings
  (deliberately factored out so a standalone test can exercise it without linking the whole backend).
- `fex_x86_64_emulator.cpp` — the backend, the syscall bridge, and the WoW64 gate-crossing/rebase
  integration.

### Guest model — the key difference from the other backends

FEX is an **in-process** binary translator. It does not sandbox a separate guest address space:
translated guest code runs inside the host process and **guest virtual addresses are host virtual
addresses** (a 1:1 mapping, subject to the WoW64 rebase described above for 32-bit processes). This
drives the whole design:

- `map_memory()` is a real `mmap(MAP_FIXED)` at the guest address; `unmap_memory()`/
  `apply_memory_protection()` are `munmap`/`mprotect`. A sorted region map tracks what is mapped, and
  (on macOS) a per-4KB-page permission shadow table reconciles the 16KB host page granularity against
  the guest's 4KB pages.
- `read_memory()`/`write_memory()` are direct host `memcpy`s once the range is confirmed mapped (the
  guest pointer is directly dereferenceable). Writes invalidate FEX's translation cache for the range
  — for a WoW64 process, both Contexts' caches, since the Context active at unmap time does not
  necessarily match the Context whose cached translations cover the unmapped range.
- `map_host_memory()` aliases caller-owned memory into the guest with `mremap(MREMAP_FIXED)` on Linux
  or `mach_vm_remap` on macOS (e.g. for the GPU bridge), and is *not* munmap'd on teardown.

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

FEX is vendored as a git submodule (`deps/FEX`) and built **in-tree** from source — there is no manual
toggle. The top-level CMake enables the backend automatically when all of these hold:

- target is **ARM64 Linux** (not Android: the NDK's libc++ lacks `std::atomic_ref`, which FEX needs)
  **or ARM64 macOS**,
- the compiler is **Clang** (FEX rejects GCC/MSVC — Apple Clang qualifies), and
- the `deps/FEX` submodule is checked out.

Otherwise the backend is silently skipped, so all other builds are unaffected. FEX is **not** added
via `add_subdirectory` — it assumes it is the top-level project (~60 uses of `CMAKE_SOURCE_DIR`) and
would configure its whole loader/tools tree. Instead `deps/CMakeLists.txt` builds it standalone with
`ExternalProject` (only the `FEXCore_shared` target → a self-contained `libFEXCore.{so,dylib}`) and
exposes it as the imported `fexcore` target that `fex-emulator` links. Embedder-hostile FEX options
(its custom allocator/jemalloc, LTO, telemetry, tools, tests) are disabled.

```sh
# On an arm64 Linux or macOS host with Clang:
git submodule update --init --recursive deps/FEX
cmake --preset=release   # FEX backend auto-enables
cmake --build --preset=release --target fex-emulator
```

**Note for `deps/FEX` changes**: FEXCore is only rebuilt when its own `ExternalProject` target is
invoked directly — a plain `cmake --build --preset=release` does *not* pick up `deps/FEX` source
changes automatically. After editing anything under `deps/FEX`, rebuild it explicitly:

```sh
ninja -C build/release/deps/fex_external-prefix/src/fex_external-build FEXCore/Source/libFEXCore.dylib
```

No copy step is needed afterward — `fex-emulator`'s rpath points directly at that build directory
(see "Build" above), so the freshly-rebuilt library is picked up in place.

`backend-selection` links `fex-emulator`, defines `SOGEN_ENABLE_FEX`, adds `backend_type::fex`, and
honors `EMULATOR_FEX=1`; the Python `Backend` enum gains `fex`.

## Selecting the backend

```sh
EMULATOR_FEX=1 ./analyzer -e root c:/test-sample.exe
```

or, from Python, `sogen.Backend.fex`.
