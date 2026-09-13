# ARM64 backend — development notes

The AArch64 guest architecture for the Unicorn backend (`sogen::unicorn::create_arm64_emulator`).
It sits alongside the existing x86_64 Unicorn backend in `src/backends/unicorn-emulator` and is
selected through `backend-selection` the same way.

Unicorn is the *only* engine that can host an AArch64 guest in sogen today: icicle is x86-only and
excluded wherever `SOGEN_ENABLE_RUST_CODE` is OFF, and no hypervisor backend (WHP, KVM) has an
AArch64 path. That also makes Unicorn the only option under emscripten, which is why the WASM build
is verified here rather than left to a later stage.

## Status

`arm64-backend-test` builds and passes 25/25 on macOS arm64. The backend is functionally complete
with no stubs: registers, memory mapping and MMIO, read/write/execute and basic-block hooks,
`svc`/`brk` instruction hooks, state serialization, and pointer authentication.

`map_mmio` and `hook_basic_block` are implemented but have no arm64 consumer yet and no arm64 test
coverage. Both are verbatim ports of the x86 implementations, which the x86 suites do exercise.

## Architecture

- `src/emulator/arm64_register.hpp` — the `arm64_register` enum, kept in lockstep with Unicorn's
  `uc_arm64_reg` (a `static_assert` pins `arm64_register::end == UC_ARM64_REG_ENDING`).
- `src/emulator/arch_emulator.hpp` — `arm64_traits`, `arm64_cpu`, `arm64_emulator` and the
  `arm64_64_emulator` alias.
- `src/backends/unicorn-emulator/unicorn_arm64_emulator.{hpp,cpp}` — the backend, plus the
  `arm64_mappable_emulator` interface it hands out (declared in the header, `:21`).
- `src/backends/unicorn-emulator/unicorn_common.hpp` — helpers shared with the x86_64 backend.
- `src/arm64-backend-test/` — the gtest suite.

Guest model: AArch64 at EL1, `UC_CPU_ARM64_MAX`, with EL3/EL2/EL1 pointer-authentication gates
opened at construction (see below).

`set_thread_pointer` / `get_thread_pointer` map to `TPIDRRO_EL0`, **not** `TPIDR_EL0`. That is a
Darwin choice baked into an otherwise generic backend: XNU keeps the thread-self pointer in the
read-only `TPIDRRO_EL0` and leaves `TPIDR_EL0` free for the process. A Linux or Windows arm64 guest
would want `TPIDR_EL0` instead, so an OS layer targeting one of those has to override this rather
than inherit it.

## Build enablement

`deps/CMakeLists.txt` sets `UNICORN_ARCH "x86;aarch64"` (it was `"x86"`).

Turning AArch64 on exposed four *pre-existing* bugs in the vendored `momo5502/unicorn` fork —
AArch64 had never compiled in it, and in fact no non-x86 target in that fork has ever linked. The
fixes are carried as configure-time patches in `cmake/unicorn-patches/`
(`0001-enable-aarch64-build.patch`, `0002-per-target-adapter-helper-names.patch`), applied in order
and idempotently by a step in `deps/CMakeLists.txt` that fails the configure loudly if it can
neither apply a patch nor confirm the tree is already patched. They are patches rather than a dirty
submodule because submodule edits do not survive `git clone --recurse-submodules` or
`git submodule update --force`.

Full root-cause analysis of all four bugs is in `docs/unicorn-aarch64-patch.md`; it is not repeated
here.

## `svc` dispatch, and why it is not an instruction hook

AArch64 has no `UC_HOOK_INSN` for `svc`. `arm64_insn_hook_validate`
(`deps/unicorn/qemu/target/arm/unicorn_aarch64.c:443-451`) accepts only `WFI`, `MRS`, `MSR`, `SYS`
and `SYSL`; anything else is rejected. `svc` instead raises `EXCP_SWI`, which surfaces through
`UC_HOOK_INTR`.

So `hook_instruction` is built on the interrupt hook and filters on the exception index
(`deps/unicorn/qemu/target/arm/cpu.h:32-38`):

| `arm64_hookable_instructions` | exception index |
| --- | --- |
| `svc` | `EXCP_SWI` = 2 |
| `brk` | `EXCP_BKPT` = 7 |

(`EXCP_UDEF` = 1 is the undefined-instruction index, relevant to the hazard below.)

## The PC contract — measured, and the two directions differ

Unicorn does **not** re-add the instruction size on the AArch64 interrupt path.
`deps/unicorn/qemu/accel/tcg/cpu-exec.c:380-431` carries pre-hook PC fixups for `TARGET_X86_64`,
`TARGET_MIPS`, `TARGET_RISCV`, `TARGET_SPARC` and `TARGET_PPC` — there is **no ARM case**.

The consequence is that the two hookable instructions behave in opposite ways:

- **`svc`**: PC is already past the instruction when the hook runs (measured `0x10004` for an `svc`
  at `0x10000`), and is left wherever the callback leaves it. A syscall dispatcher writes its
  results and returns **without touching PC**. This differs from the x86 syscall hook, where
  Unicorn adds the instruction size back afterwards.
- **`brk`**: `gen_exception_bkpt_insn` uses `pc_curr`, so PC stays **on** the faulting instruction.
  A `brk` handler that wants to resume past it must advance PC by 4 itself, or it will loop.

`instruction_hook_continuation` is a no-op on this backend in both cases: by the time the hook runs
the trap has already replaced the instruction, so there is nothing to skip or re-execute.

## Fault suppression: a hazard for the future OS layer

Registering *any* `UC_HOOK_INTR` callback sets Unicorn's `catched` flag unconditionally for every
hook invoked (`cpu-exec.c:406-424`) — the flag is set per callback dispatched, with no regard for
whether that callback actually handled the exception index. When `catched` is true Unicorn skips
the `uc->invalid_error = UC_ERR_EXCEPTION` / `cpu->halted = 1` block, so **every** exception index
below `EXCP_INTERRUPT` is silently swallowed, including indices the hook explicitly filters out and
returns from.

This is worse on AArch64 than on 32-bit ARM. `unicorn_arm.c:791` assigns
`uc->stop_interrupt = arm_stop_interrupt`, which routes undefined instructions to the
`UC_HOOK_INSN_INVALID` path instead. AArch64's `uc_init`
(`unicorn_aarch64.c:526-541`) never assigns `stop_interrupt`, so it stays NULL, the
`cpu->uc->stop_interrupt && ...` test at `cpu-exec.c:344` short-circuits, and `EXCP_UDEF`,
`EXCP_PREFETCH_ABORT` and `EXCP_DATA_ABORT` all funnel into the swallowing block.

Measured: an undefined instruction throws with no hooks registered, and is silently swallowed once
an unrelated `svc` hook exists.

**The mitigation now exists.** `macos_emulator::setup_hooks` installs a catch-all interrupt hook
alongside its `svc` hook — in the same place, because the `svc` hook is what disarms the reporting —
and stops the CPU with `stop_reason::unhandled_cpu_exception` on any index other than `EXCP_SWI`.
Any other OS layer built on this backend needs the same thing. See `docs/macos-kernel-core.md`,
"Fault suppression, and why the catch-all hook exists".

## Pointer authentication

PAC works. Measured round trip: `original = 0x0000000100003F00` → `pacia` → `0xD44D000100003F00`
→ `autia` → `0x0000000100003F00`.

The CPU model was never the obstacle — `UC_CPU_ARM64_MAX` already reports
`ID_AA64ISAR1_EL1 = 0x0000011101211012` (`APA = 1`), and no `cpu64.c` patch was needed. What gates
PAC is three registers that QEMU resets to zero and that real firmware would have set. All three
are programmed by `enable_pointer_authentication()` at construction:

| Register | Bits | Effect if clear |
| --- | --- | --- |
| `SCTLR_EL1` | `EnIA`, `EnIB`, `EnDA`, `EnDB` | PAC instructions decode as a silent no-op |
| `HCR_EL2` | `API`, `APK` | `EXCP_UDEF` from `pauth_check_trap` |
| `SCR_EL3` | `API`, `APK` | `EXCP_UDEF` from `pauth_check_trap` |

**`SCR_EL3.RW` is load-bearing and non-obvious.** Without it, `arm_hcr_el2_eff()` treats EL2 as
AArch32 and masks everything above bit 31, which takes `HCR_EL2.API` (bit 41) with it. Measured:
`SCR = 0x30031` traps, `SCR = 0x30431` passes. `set_system_register_bits` therefore reads each
register back after writing and throws if QEMU masked the bits away, rather than letting PAC
degrade silently into a no-op.

The three registers are programmed once, at construction, and nothing re-applies them afterwards —
`restore_registers()` and `deserialize_state()` are byte-for-byte ports of their x86 counterparts.
PAC therefore survives a snapshot restore only because Unicorn's `uc_context` covers AArch64 system
registers. `SctlrEl1SurvivesBareContextRoundTrip` in `src/arm64-backend-test/` pins exactly that, so
a Unicorn bump that dropped them fails a test instead of silently disabling PAC on every restore.

Two residual divergences from real hardware worth recording:

1. **FPAC is not emulated.** A failed `AUT*` poisons the pointer instead of faulting at the
   authentication site, so the fault surfaces later, at the next dereference. Expect PAC failures
   to present as a data abort at a nonsense address rather than as an authentication fault.
2. **`ID_AA64ISAR1` reports `APA = 1` / `API = 0`** — the inverse of real Apple silicon. An OS layer
   must answer PAuth feature queries from its own tables rather than forwarding the guest CPU's ID
   registers, or it will describe the wrong PAC variant to the guest.

## Known emulator quirk: stale translation blocks

Rewriting guest code with *identical bytes* does not reliably invalidate Unicorn's cached
translation blocks, so re-executing at a reused guest address can return a stale decode. It is
reproducible and deterministic.

This matters more here than it would for most emulators: self-modifying and packed code is a
primary use case for a malware-analysis tool. Unfixed.

## WebAssembly

**Verified working.** `unicorn-emulator`, including `libaarch64-softmmu.a`, builds clean under
emscripten with `SOGEN_EMSCRIPTEN_MEMORY64=ON`:

```sh
cmake --preset=emscripten64
cmake --build --preset=emscripten64 --target unicorn-emulator
```

The AArch64 target compiles under `CONFIG_TCG_INTERPRETER` (TCI), which is architecture-neutral, so
there was no TCI-side blocker. This discharges the WASM half of the engine decision: the
browser-hosted configuration (`SOGEN_ENABLE_RUST_CODE` OFF, no hypervisor) can host an AArch64
guest.

One real portability bug surfaced and was fixed. Emscripten 4.0.23 ships a newer clang than the
macOS host toolchain and enables `-Wmissing-designated-field-initializers`, which under the
project's `-Werror` rejected the four `uc_arm64_cp_reg` descriptors in `unicorn_arm64_emulator.cpp`
for omitting `.val`. They now initialize `.val = 0` explicitly; the field is a don't-care that
`read_system_register` / `set_system_register_bits` overwrite at use. Nothing about this was
emscripten-specific beyond the compiler version — any newer clang would have caught it.

### Local emscripten setup

`CMakePresets.json` resolves the toolchain through
`$env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`, which assumes an
emsdk-style install. A Homebrew emscripten has no `upstream/emscripten` level — the toolchain file
sits directly at `/opt/homebrew/opt/emscripten/libexec/cmake/...`. Point `EMSDK` at a small shim
directory containing an `upstream/emscripten` symlink rather than editing `CMakePresets.json`,
which must keep working for the emsdk-based setup CI and other developers use:

```sh
mkdir -p /tmp/emsdk-shim/upstream
ln -sfn /opt/homebrew/opt/emscripten/libexec /tmp/emsdk-shim/upstream/emscripten
EMSDK=/tmp/emsdk-shim cmake --preset=emscripten64
```

## Duplicate `_adapter_helper_*` symbols — fixed

Enabling the AArch64 target originally produced **246 distinct**
`ld: warning: duplicate symbol '_adapter_helper_*'` warnings between `libx86_64-softmmu.a` and
`libaarch64-softmmu.a`. Apple's `ld` only warns, so macOS builds appeared to succeed; `ld.lld`
rejects the same shape outright, so **the Linux CI legs would have failed at link time**.

It was also a latent correctness bug rather than cosmetic noise. The thunks' bodies call
`HELPER(name)`, which the per-target `-include <arch>.h` remaps to a target-suffixed symbol — so
each target emitted a same-named thunk with a *different* body and the linker silently kept one.

Fixed by `cmake/unicorn-patches/0002-per-target-adapter-helper-names.patch`, which gives the thunks
per-target names via a new `ADAPTER_HELPER(name)` macro in `helper-head.h`, using the
`UNICORN_ARCH_POSTFIX` mechanism the fork already applies to `address_space_*` in `memory.h`. Full
analysis, including why re-enabling the disabled `IS_GLOB` deduplication and why marking the thunks
`weak` were both rejected, is bug 4 in `docs/unicorn-aarch64-patch.md`.

Verified: zero duplicate warnings, `nm` shows `_adapter_helper_atomic_add_fetchb_x86_64` /
`..._aarch64`, `arm64-backend-test` 25/25 and `linux-emulator-test` 38/38 pass, and `wasm-ld` links
a real executable extracting *both* targets' `translate.c.o` with no
`--allow-multiple-definition`.

## Build & test

```sh
cmake --build --preset=release --target arm64-backend-test
cd build/release/artifacts && ./arm64-backend-test
```

`arm64-backend-test` is registered with CTest (`add_test` in `src/arm64-backend-test/CMakeLists.txt`),
so `ctest` picks it up automatically on every platform CI runs tests on, including `macos-latest`.
It needs no emulation root, no fixture and no downloaded artifact beyond its own executable.

### clang-tidy

`unicorn-emulator` is **not** a clang-tidy target — backends are added through
`sogen_add_subdirectory_and_get_targets("backends" ...)` and are not part of the `OWN_TARGETS` list
that `CMakeLists.txt:146` passes to `sogen_targets_enable_clang_tidy`. `arm64-backend-test` and
`backend-selection` *are* checked.

CI pins LLVM 21 for the tidy job. A newer local clang-tidy reports checks that do not exist in 21
(`readability-redundant-typename`) or that pre-existing CI-green code trips identically
(`cppcoreguidelines-pro-bounds-avoid-unchecked-container-access`, which also fires in
`src/linux-emulator-test/emulation_test.cpp`). When running the tidy preset locally, match CI's
LLVM version before treating a diagnostic as a regression, and note that `find_program` will
silently warn and skip clang-tidy entirely if it is not on `PATH` — pass
`-DCLANG_TIDY_EXECUTABLE=` explicitly to be sure it actually ran.
