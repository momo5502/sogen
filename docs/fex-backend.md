# FEX backend — development notes

The FEX-Emu emulator backend (`sogen::fex`). A standalone backend that mirrors the structure of the
other backends, selected at runtime via `EMULATOR_FEX=1`. Its functional targets are **macOS on Apple
Silicon** and **Android on AArch64** — [FEX](https://fex-emu.com) only JITs x86/x86-64 to ARM64.

## Status

Working on Darwin/Apple Silicon: `test-sample.exe` runs to completion, matching the Unicorn
backend's behavior. Android/AArch64 is now a functional target and currently requires a 4KB host
page. Other AArch64 Linux hosts have build coverage only and are not expected to run.

## Security / address-space model

**FEX shares the guest and host address space — there is no isolation between them.** Unlike
Unicorn/Icicle (a fully software-simulated guest address space, sandboxed from the host process by
construction) or KVM/WHP (real hardware virtualization, a genuinely separate guest physical address
space), FEX is an in-process JIT: it translates x86/x86-64 to ARM64 and executes the result directly
inside this process, treating guest virtual addresses as host virtual addresses (a 1:1 mapping,
`guest VA == host VA`). Concretely:

- `map_memory()` is a real `mmap(MAP_FIXED)` at the guest address; `read_memory()`/`write_memory()`
  are a direct host `memcpy` once the range is known to be mapped — there is no page-table or bounds
  check standing between a guest access and this process's own memory.
- A guest out-of-bounds read or write can therefore land directly in this process's own memory —
  including sogen's own runtime state — in a way that is structurally impossible under
  Unicorn/Icicle. This is an inherent property of how FEX-Emu is designed to work (it is built as a
  transparent Linux binfmt-style translation layer, the same category as `qemu-user`/`box64`, where
  "guest pointer is a host pointer" is exactly what makes syscall passthrough and native-speed
  execution possible), not a gap specific to this port. See
  [`FEX-Emu/FEX`'s own `docs/ProgrammingConcerns.md`](https://github.com/FEX-Emu/FEX/blob/main/docs/ProgrammingConcerns.md)
  and `docs/allocator_usage.md`, which state this plainly from FEX's own side.
- This backend's own address-space protections (`host_reserved` region tracking in
  `windows-emulator/memory_manager.cpp`, the Apple-only FEXCore internal arena, and `GDT_ADDR`'s
  placement in `process_context.hpp`) are mitigations, not a sandbox: they keep ordinary guest
  allocations from *landing on* addresses this backend or the host process needs, reducing collision
  risk. They do not and cannot stop a guest from computing an arbitrary pointer into any other valid
  host-mapped memory — there is no hard fault boundary for that, by design.
- Practically, this means FEX is not an appropriate backend for running guest code you do not trust
  at all to stay within its own memory — the same class of workload Unicorn/Icicle's sandboxed model
  is suited for.
- `deps/CMakeLists.txt` explicitly disables FEX's own internal allocator
  (`-DENABLE_FEX_ALLOCATOR=OFF`, `-DENABLE_JEMALLOC_GLIBC_ALLOC=OFF`) — guest allocation is owned by
  sogen's memory manager. On Apple, the backend separately installs FEXCore allocator hooks that
  steer FEXCore's own anonymous internal mappings into a dedicated 4GiB host-reserved arena, keeping
  them out of guest allocation ranges. This is still only a collision mitigation, not isolation.
  WoW64/32-bit processes remain unsupported by this backend.

## Architecture

- `fex_x86_64_emulator.hpp`/`.cpp` — factory `sogen::fex::create_x86_64_emulator()` and the backend
  itself; see the file's own top-of-file comment for the implementation-level version of the model
  described above.
- `fex_x86_64_common.hpp` — register classification (`classify_gpr` and friends) mapping
  `x86_register` to FEXCore's `CPUState`.
- Guest `syscall` instructions route back to sogen through a `FEXCore::HLE::SyscallHandler`, which
  invokes the registered syscall instruction-hook — the same mechanism the Windows emulation layer
  uses for every backend.
- The fine-grained `hook_memory_read/write/execution/range_execution` and `hook_basic_block` hooks
  are accepted for API compatibility but never fire, exactly like the KVM backend: guest code runs
  natively, so there is no per-access/per-instruction instrumentation point short of single-stepping.
- `deps/FEX` is pinned to a personal fork (`github.com/JackTYM/FEX`), not upstream FEX-Emu directly —
  carries a 10-submodule removal (376MB trim) and a couple of small Darwin-portability/opdispatch
  patches; see the fork's own commit history for the exact list. **Pinning to a fork means these
  patches don't automatically track upstream FEX-Emu security fixes** — periodically rebasing onto a
  newer upstream release is a maintenance cost worth weighing.

## Build & test

No special setup needed, unlike the KVM backend (which requires a Docker-on-Windows workaround to
build or run at all): from an ARM64 Clang host (Apple Silicon macOS or ARM64 Linux), the ordinary
`cmake --preset=release` picks this backend up automatically once `deps/FEX` is checked out (see the
root `CMakeLists.txt` gate). Run with `EMULATOR_FEX=1`. Android currently requires a 4KB host page.

## Known limitations

- **On Darwin, a protection fault from a plain `STR`/`STUR` store can be misclassified as a read.**
  `decode_arm64_store` deliberately recognizes only the `STLR` family; a plain store to read-only
  memory can therefore surface as an unhandled host signal instead of a guest
  `STATUS_ACCESS_VIOLATION`, while an unmapped plain store reaches the guest with the operation marked
  as a read. Android avoids this specific misclassification by using the kernel-provided ESR WnR bit.
- **`sync_host_page_apple`'s permission union can strip `PROT_EXEC` or over-grant write access near
  PE section boundaries.** Guest permissions are tracked per-4KB shadow slot but applied per-16KB
  Apple host page (four slots share one host page); when slots sharing a host page disagree, the
  code unions them (documented in-code as a deliberate, temporary simplification pending a later
  Mach-exception-handler phase that can resolve per-slot faults properly). A PE's `.text` (RX)
  immediately followed by `.data` (RW) landing in the same host page can lose exec permission on the
  tail of the code page — a real execution fault, not just an over-permissive read-only page.
- **The MMIO fault path isn't fully async-signal-safe.** `handle_mmio_fault` calls the registered
  MMIO callback directly from inside the real signal handler, rather than through this file's own
  `pending_fault_dispatch_` mechanism (built for exactly this class of hazard, and already used for
  `memory_violation_hooks_`/`interrupt_hooks_`). The one production registrant, `kusd_mmio::read`,
  takes a mutex also used by `kusd_mmio::access()` from normal call context. No live self-deadlock
  path exists today under the current single-cooperative-thread-per-vCPU model, but this becomes a
  real hazard once multi-vCPU FEX support matures.
- **A smaller, same-bug-class gap remains:** `commit_memory` guards against committing into
  `section_kind`/`host_reserved` regions but has no equivalent guard for a guest probing directly into
  other backends' reserved ranges.

## Fixes found during bring-up and review

- **Host-reserved-address-space tracking under Apple's `mmap(MAP_FIXED)` semantics.** Two real
  memory-safety bugs: an MMIO-region-insertion path could violate the host-reserved overlap-tracking
  invariant (undetected until a `mmap(MAP_FIXED)` failure crashed the analyzer), and a
  decommit→recommit window could let a foreign host allocation land in a guest's still-reserved
  range. Fixed by properly carving host-reserved holes and adding
  `memory_interface::release_guest_address_range()` to distinguish decommit (keep the address
  claimed) from release (actually free).
- **Scheduler-quantum-race** (backend-agnostic, extracted to its own fix — see `windows_emulator.cpp`
  `start()`'s quantum-timer watchdog): a non-atomic `switch_thread = true` / `cpu.stop()` pair could
  be split across the scheduler's own consume step, misread as a fatal crash. This backend's
  cooperative page-protect stop makes the race hot enough to hit reliably, which is how it was found.
- **`InterruptFaultPage` unwind atomicity.** A raced interrupt-fault-page unwind (the mechanism this
  backend uses to interrupt JIT-compiled code) could be misread as a fatal stop by `start()`'s run
  loop under concurrent access; tagged and re-armed instead of terminating.
- **`int 2Dh` (the Windows debug-service trap) resumed at the wrong address.** FEXCore only reports
  RIP already past the trapping instruction for real `INT3`/`INT1` (`SetRIPToNext` in FEXCore's
  `OpcodeDispatcher.cpp`); a generic `INT n` this backend can't dispatch directly (remapped from a
  synthetic `#GP` back to its own vector) already reports the instruction's own start address. A
  shared-code fixup that unconditionally corrected for the INT3 case broke this one. Fixed by
  normalizing in this backend, conditioned on FEXCore's `TrapNo == X86_TRAPNO_BP` (verified to be set
  in exactly one place in all of FEXCore — the real `0xCC` case) — which let the shared-code
  workaround be deleted outright rather than patched further.
- **Missing GPR sub-register widths.** `classify_gpr` had no cases for `r8b`-`r15b`/`r8w`-`r15w`/
  `r8d`-`r15d` (only the full 64-bit and the original 8 registers at every width) — silently
  zero-filled on read, silently dropped on write. Common in real x86-64 code (e.g. `mov r9d, eax`).
- **`setup_gdt` discarded an allocation failure.** `GDT_ADDR` is deliberately placed at a high, fixed
  address on Apple specifically because low addresses and a range of typical host allocations are
  unusable/collision-prone here (see the address-space model above) — a collision there is real, not
  theoretical, and previously failed silently instead of raising a diagnosable error.
