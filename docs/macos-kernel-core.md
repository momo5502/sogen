# Darwin kernel core — development notes

Stage 3 of the macOS arm64 emulation programme: the BSD syscall layer that sits between the Mach-O
loader (`docs/macho-loader.md`) and the arm64 Unicorn backend (`docs/arm64-backend.md`). On its own it
takes a mapped Darwin image and runs it through `write` and `exit`; the stages built on top of it since —
Mach IPC, concurrency, memory, the window server — take a real GUI application all the way to a composed,
interactive window. See [`macos-emulation.md`](macos-emulation.md) for the current end-to-end picture.

This document records the parts that are not recoverable by reading `src/macos-emulator/`: the ABI
facts that forced the design, the Unicorn behaviours that shaped it, and the gaps a later stage
inherits. It does not restate the Mach-O loader or the backend; both have their own document.

## Status

Working: `svc` dispatch, 109 of 456 xnu-12377.121.6 BSD syscalls across
`syscalls/{process,io,file,memory,sysctl,time}.cpp` — measured by `macos-emulator-test`'s ratcheted
coverage test, `[ COVERAGE ]` in its output — the two-page commpage, `sysctl` by MIB and by name, the
Darwin initial stack (`argc`/`argv`/`envp`/`apple[]`), the guest→host filesystem and fd table, and
`macos_memory_manager` with snapshot serialization. `MacosHelloWorld` still runs a hand-written arm64
Mach-O through `write` and `exit` on this layer alone; a real GUI application now runs on top of it all
the way to a composed window (see [`macos-emulation.md`](macos-emulation.md) for what and how much).

What this document once listed as "deliberately absent, owned by a named later stage" has mostly landed
since: Mach IPC and `mach_msg` (Stage 4 — this layer still answers every negative `x16` other than
`-3`/`-4` with `ENOSYS` by name) is [`macos-mach-ipc.md`](macos-mach-ipc.md); dyld linking is real, not
stubbed — sogen maps, dyld links (`docs/macho-loader.md`); threading, the workqueue pool and kqueue
timers are [`macos-concurrency.md`](macos-concurrency.md). Sockets remain refused by design, not
missing — `sys_socket` accepts only `AF_UNIX` (see `macos-emulation.md`, "What does not work"). Signal
delivery to a registered handler is still not modelled: `sigaction`/`sigprocmask` only record the
handler and mask (`macos_process_context::signal_actions`), and nothing yet turns a raised exception
into a call to it.

## The syscall ABI

The syscall number is in **`x16`**, not in the `svc` immediate. Darwin arm64 issues `svc #0x80`
uniformly, so **there is no class-byte encoding on arm64**. The x86-64 convention of encoding the
class (`0x2000000` BSD, `0x1000000` Mach, `0x3000000` machdep) in the high bits of the syscall number
does not apply here, and a table transcribed from an x86-64 reference will be wrong.

`bsd_syscall_dispatcher::dispatch` reads `x16` as a signed 32-bit value and branches on its **sign**:

| `x16` | meaning |
| --- | --- |
| `0x80000000` (`INT32_MIN`) | `platform_syscall` |
| negative | Mach trap, table indexed by `-x16` |
| `0` | indirect syscall: the real number is in `x0`, arguments shift up by one register |
| positive | BSD syscall |

The `0x80000000` comparison must come **first**, exactly as xnu orders it. `INT32_MIN` negates into
itself, so a Mach-trap branch taken before the sentinel test would index the trap table with a
nonsensical value. `MacosSyscallDispatch.PlatformSyscallSentinelIsNotTreatedAsAMachTrap` pins that
ordering.

Arguments are `x0`–`x7` only. Darwin arm64 never spills syscall arguments to the stack, so
`get_macos_syscall_argument` treats an out-of-range index as a fault rather than reading memory.

### Errors travel in the carry flag

Darwin does **not** use Linux's `-errno` return convention. Success clears **`NZCV.C`**; failure sets
it and puts a **positive** errno in `x0`. libSystem's `cerror` stubs branch on `b.lo`, so a handler
that returns `-EINVAL` hands the caller `-22` as a *successful* result rather than setting `errno`.

`write_macos_syscall_result` and `write_macos_syscall_error` are the only two ways a handler may
return, which is what keeps the Linux convention from leaking in. `write_macos_syscall_error` warns
if it is handed a negative value rather than silently negating it.

`dispatch` clears the carry flag *before* the handler runs, mirroring xnu's trap path: a syscall that
writes no result at all still reports success.

## The PC contract

Unicorn does not re-add the instruction size on the AArch64 interrupt path (see
`docs/arm64-backend.md`, "The PC contract"). The two hookable instructions therefore behave in
opposite directions, and this layer depends on it:

- **`svc`** — PC already points past the instruction when the hook runs. Syscall handlers must **not**
  touch PC. `instruction_hook_continuation::skip_instruction` is returned for uniformity with the
  x86-64 dispatcher; it is a no-op on this backend.
- **`brk`** — PC stays **on** the faulting instruction. A future debugger that wants to resume past a
  breakpoint has to advance PC by 4 itself, or it will loop.

## Fault suppression, and why the catch-all hook exists

Registering *any* `UC_HOOK_INTR` callback sets Unicorn's `catched` flag once per callback **invoked**,
not per exception **handled**, and AArch64 leaves `uc->stop_interrupt` NULL. The consequence is that
installing the `svc` hook disarms Unicorn's own fault reporting for every exception index below
`EXCP_INTERRUPT` — `EXCP_UDEF`, `EXCP_PREFETCH_ABORT` and `EXCP_DATA_ABORT` included, and including
indices the hook explicitly filters out and returns from. `docs/arm64-backend.md` has the measurement
and the Unicorn source references.

**The failure mode is a hang, not an error.** The guest re-enters the faulting PC forever: an
unbounded run never returns, and a counted run burns its entire budget on one instruction that never
retires. That is why every test around this area carries a `timeout`, and why
`MacosFaultSuppression.BareBackendStillRaisesOnUndefinedInstruction` exists as a control pinning the
premise — it holds a bare backend with no hooks and asserts that the undefined instruction *does*
still raise.

The mitigation is in `macos_emulator::setup_hooks`: the catch-all interrupt hook goes in together
with the `svc` hook, not later, and records `stop_reason::unhandled_cpu_exception` for any index that
is not `EXCP_SWI`. A separate memory-violation hook records `stop_reason::unhandled_memory_violation`
before the same fault surfaces a second time as `UC_ERR_*_UNMAPPED` out of `uc_emu_start`; recording
it early is what stops the second report from overwriting the reason with `backend_error`.

## The commpage

Two pages, unlike x86-64's single flat page: `_COMM_PAGE64_BASE_ADDRESS` at `0xFFFFFC000` and the
read-only `_COMM_PAGE64_RO_ADDRESS` at `0xFFFFF4000`. The `RO_`-prefixed offsets in
`commpage_offset` are relative to the second page — the page-shift fields live there and only there
(`MacosCommpage.PageShiftsLiveOnTheReadOnlyPageOnly`).

Both bases are 16 KiB aligned, which was computed rather than assumed.

`_COMM_PAGE_USER_TIMEBASE` **must be `1` (`USER_TIMEBASE_SPEC`)**. libSystem branches on this byte to
pick the counter register it reads inline: `NOSPEC` (2) reads `CNTVCTSS_EL0` and `NOSPEC_APPLE` (3)
reads the Apple IMPDEF `S3_4_C15_C10_6`. The emulated CPU implements neither, so either value faults
inside libSystem. `SPEC` reads `CNTVCT_EL0`, which it does implement.

The field table carries no `numer`/`denom` pair, so nothing here answers `mach_timebase_info`; a guest
that asks for the ratio goes through a Mach trap, which Stage 4 owns. `TIMEBASE_OFFSET` and
`CONT_TIMEBASE` are offsets, not a ratio.

`0xFC0000000`–`0xFFFFFFFFF` is reserved as the commpage nesting region and is excluded from the mmap
range (`MACOS_MAX_MMAP_END_EXCL` is `MACOS_COMMPAGE_NESTING_START`), so a guest allocation can never
land on top of the commpage.

## `sysctl` and PAuth

Feature queries are answered from `macos_system_info`, **never** by reading `ID_AA64ISAR1_EL1`. The
CPU model Unicorn exposes (`UC_CPU_ARM64_MAX`) advertises features the emulator does not actually
implement end to end, so routing `hw.optional.arm.FEAT_*` through the real register would tell the
guest to use instruction sequences that then fault. `macos_system_info` is constructed once by
`macos_emulator` and read by both `sysctl` and the commpage, which is what keeps `hw.memsize` and
`_COMM_PAGE_MEMORY_SIZE` from drifting apart — a guest that cross-checks the two notices any
divergence, and `MacosSysctl` fails if they do.

## What was hoisted out of `linux-emulator`

`guest_file_system`, `guest_fd_table` and `guest_memory_object` moved to `src/common/guest/` so both
OS layers share one definition. Linux call sites were left untouched by alias shims
(`using linux_file_system = guest_file_system;` and friends) rather than by a mechanical rename;
`linux-emulator-test` is the independent gate on the hoist having been transcription-clean.

There is no `macos_file_system.hpp`. After the hoist it would contain nothing but an alias, so
`macos_emulator` uses `guest_file_system` directly. The file is worth adding the first time a
genuinely Darwin-specific path rule exists.

Deliberately **not** hoisted: the thread scheduler, sockets, and `procfs`. The scheduler and sockets
are entangled with Linux futex and signal semantics that Darwin does not share, and `procfs` has no
Darwin counterpart at all — Darwin exposes the same information through `sysctl` and `proc_info`.

`read_system_register` was put on `arm64_cpu<Traits>` rather than on `cpu_interface`, deviating from
the design spec. `cpu_interface` is the architecture-neutral abstraction shared with the x86-64
Unicorn, icicle, WHP and KVM backends; an AArch64 system-register accessor is dead weight on four of
them. `set_thread_pointer` sits there for the same reason.

## Address-space layout

| Range | Contents |
| --- | --- |
| `0x000000000`–`0x0FFFFFFFF` | `__PAGEZERO`, never mapped |
| `0x100000000` | main executable base |
| `0x16F400000`–`0x16FC00000` | main thread stack (8 MiB, top at `kern.usrstack64`) |
| `0x180000000`–`0x2E6440000` | dyld shared cache |
| `0x300000000`–`0xFBFFFFFFF` | mmap range |
| `0xFC0000000`–`0xFFFFFFFFF` | commpage nesting reservation, end of the address space |
| `0xFFFFF4000` | commpage, read-only page (inside the reservation) |
| `0xFFFFFC000` | commpage, main page (inside the reservation) |

Page size is 16 KiB throughout (`MACOS_PAGE_SIZE`), which is also the allocation granularity.
`0x16FC00000` was read from a real host with `sysctl kern.usrstack64` rather than assumed.

## Known gaps

Inherited by Stage 5 rather than rediscovered by it.

1. **`kNumCPUs` in `_COMM_PAGE_CPU_CAPABILITIES`.** The mask and shift were never resolved. The
   commpage therefore reports the measured host's self-consistent 14-CPU capability word rather than
   a synthesized single-CPU one. A guest that spawns one worker per reported CPU will over-spawn
   against a single-vCPU emulator. Resolve this before Stage 5.

2. **`apple[]` carries only `executable_path=` and `stack_guard=`.** The rest of the key set is
   unverified and belongs to Stage 5.

3. **File-backed `mmap` reads into anonymous pages.** `MAP_SHARED` silently gets `MAP_PRIVATE`
   semantics and a guest's writes never reach the host file. Making it zero-copy needs
   `map_host_memory` (native) or a page provider (WASM).

   **Unicorn has no copy-on-write.** `uc_mem_map_ptr` aliases the host buffer directly, so that
   upgrade must source its buffer from a host `mmap` with `MAP_PRIVATE` — otherwise emulated guest
   code writes straight through into the user's real macOS system files.

   Relatedly, the Mach-O mapper does not unwind a partial mapping when a later segment fails. A
   failed `load_application` leaves the address space occupied and the emulator unusable, so anything
   that wants to retry a mapping must make `map_macho_from_data` transactional first.

4. **`csops` fails for every operation and `ioctl` returns `ENOTTY` for every request.** Both are
   honest stubs pending a trace of what dyld actually asks for; guessing would be worse than failing.

5. **Stale translation blocks on identical-byte rewrites.** Unicorn caches translated blocks, so two
   different syscalls placed at one address replay the first. See `docs/arm64-backend.md`, "Known
   emulator quirk: stale translation blocks" — it applies here unchanged.

6. **`getattrlistbulk` is not implemented.** It is present in dyld's stub table but never executed on
   any measured path to `main()`; it answers `ENOTSUP` and names itself rather than guessing at a
   directory-enumeration format nothing has exercised yet (`src/macos-emulator/syscalls/dyld_support.cpp`).

## Build & test

```sh
cmake --build --preset=release --target macos-emulator-test linux-emulator-test arm64-backend-test
cd build/release/artifacts
timeout 300 ./macos-emulator-test
timeout 300 ./linux-emulator-test
timeout 300 ./arm64-backend-test
```

`macos-emulator-test` needs no emulation root, no sample and no downloaded artifact — every fixture
is built in-process or in a temp directory. It is registered with CTest, so CI's `ctest` legs pick it
up. `linux-emulator-test` is the regression gate on the hoist; `arm64-backend-test` on the backend
changes.

`macos-emulator` and `macos-emulator-test` are in the clang-tidy target set, and CI's tidy leg runs on
Linux and Windows — so both must compile and stay clean on platforms that are not Darwin. See
`docs/arm64-backend.md`, "clang-tidy", for the LLVM-version caveats when running that preset locally:
the `Checks:` list is written with wildcards, so a newer local clang-tidy silently enables checks CI
does not have and buries the real findings.
