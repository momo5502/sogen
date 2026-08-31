# Unicorn AArch64 patch — development notes

`deps/unicorn` (`https://github.com/momo5502/unicorn.git`, branch `dev`) never
successfully built its `aarch64-softmmu` target before sogen's Task 1 of the
ARM64 guest architecture plan flipped `UNICORN_ARCH` from `"x86"` to
`"x86;aarch64"`. Four independent, pre-existing bugs in the vendored source
surfaced as a result — none of them related to sogen's own build
configuration. They're carried as patch files in `cmake/unicorn-patches/`
(`0001-enable-aarch64-build.patch` for bugs 1-3,
`0002-per-target-adapter-helper-names.patch` for bug 4), applied in order at
configure time by `deps/CMakeLists.txt` (before `add_subdirectory(unicorn)`),
rather than committed as a dirty submodule (submodule edits don't survive
`git clone --recurse-submodules` or `git submodule update --force`). Each is
applied idempotently: the step first checks whether the patch already reverses
cleanly, and fails the configure loudly if it neither applies nor is already
applied.

Pinned commit at the time this patch was written:
`d23810735f792db1977327f3609fa3fa0446ede1`. Confirmed (via `git fetch origin dev`)
there is no newer commit on that branch, so there is no upstream fix to pull in
instead.

## Status

`arm64-backend-test` builds and passes (`uc_open(UC_ARCH_ARM64, ...)`,
`uc_arch_supported(UC_ARCH_ARM64)`) with the patch applied. `libaarch64-softmmu.a`
links successfully alongside the pre-existing `libx86_64-softmmu.a`.

No non-x86 target in this fork — arm, mips, sparc, m68k, ppc, riscv, s390x,
tricore, all nominally listed in unicorn's own default `UNICORN_ARCH` — has
ever actually linked. aarch64 is presumably just the first one anyone tried;
bug 3 below (missing helper-adapter wiring) affects every one of them
identically. **Upstreaming these fixes to `momo5502/unicorn` is the preferred
long-term resolution**; this patch is a stopgap that lets the sogen ARM64
plan proceed without blocking on a third-party merge.

## Bugs fixed

### 1. `qemu/exec.c`, `cpu_watchpoint_address_matches`: missing `uc` local

**Symptom:** compile error, `use of undeclared identifier 'uc'`, in the
`TARGET_PAGE_SIZE` macro expansion.

**Root cause:** `TARGET_PAGE_SIZE` expands to `uc->init_target_page->mask` on
any target with `TARGET_PAGE_BITS_VARY` defined. ARM has variable page sizes
(4K/16K/64K) and defines it (`target/arm/cpu-param.h:25`); x86 has a fixed
page size and takes a different branch that never needs `uc`, so this was
never exercised. `cpu_watchpoint_address_matches` never declared a local
`uc`.

**Why the fix is correct:** four *other* functions in the same file
(`phys_page_set`, `phys_page_find`, `address_space_lookup_region`,
`iotlb_to_section`, `cpu_memory_rw_debug`) already declare
`struct uc_struct *uc = cpu->uc;` guarded by `#ifdef TARGET_ARM` for exactly
this reason. The fix adds the identical declaration to the one function that
was missed — it isn't a guess about ARM semantics, it's copying an
already-established, already-working pattern from the same translation unit.

**x86 regression evidence:** none possible — the added code is compiled out
entirely (`#ifdef TARGET_ARM`) for the x86 target.

### 2. `qemu/exec.c`, `cpu_check_watchpoint`: target-specific call in target-agnostic code

**Symptom:** compile error, `call to undeclared function 'raise_exception_ra_aarch64'`.

**Root cause:** the function called `raise_exception_ra(cpu->env_ptr,
EXCP_DEBUG, ra)` unconditionally. `raise_exception_ra` is a **per-target**
symbol (renamed via macro to e.g. `raise_exception_ra_x86_64` /
`raise_exception_ra_aarch64`). i386's has signature `(env, int, uintptr_t)`;
ARM's has signature `(env, uint32_t excp, uint32_t syndrome, uint32_t
target_el, uintptr_t ra)` — 5 arguments, declared in
`target/arm/internals.h`, which `exec.c` doesn't even include. The call only
ever type-checked for i386.

**Why the fix is correct:** traced what `raise_exception_ra` actually does
for i386 — `raise_interrupt2()` sets `cs->exception_index = intno` and calls
`cpu_loop_exit_restore(cs, retaddr)`. Confirmed via `cpu_handle_exception` in
`accel/tcg/cpu-exec.c` that `EXCP_DEBUG` is handled entirely through the
generic `cpu->exception_index >= EXCP_INTERRUPT` branch, which dispatches
straight to `cpu_handle_debug_exception()` → `cc->debug_excp_handler(cpu)`
(a per-target callback both x86 and ARM already register in their CPU
class — `breakpoint_handler` and `arm_debug_excp_handler` respectively).
That branch never reads the i386-specific `env->error_code` /
`exception_is_int` / `exception_next_eip` fields that `raise_interrupt2`
also sets, nor does it depend on the i386-only `check_exception()`
prioritization `raise_interrupt2` runs first. The fix —
`cpu->exception_index = EXCP_DEBUG; cpu_loop_exit_restore(cpu, ra);` — is
the target-agnostic equivalent, and matches the exact same idiom already
used unconditionally three times in `cpu-exec.c` itself
(`cpu->exception_index = EXCP_DEBUG` / `EXCP_HALTED` / `EXCP_INTERRUPT`).

**x86 regression evidence:** `linux-emulator-test` (38/38 pass) exercises
real x86-64 TCG execution — syscalls, signals, futexes, mmap, ELF loading —
through this exact `exec.c`/`cpu-exec.c` code path.

### 3. ARM target never wired up TCG helper-adapter generation

**Symptom:** link error, ~150 undefined `_adapter_helper_*` symbols
(vfp/neon/crypto helper glue referenced by `all_helpers[]` in `tcg/tcg.c`).

**Root cause:** `include/exec/helper-gen.h`'s `DEF_HELPER_FLAGS_N` macros
call `GEN_ADAPTER_N(name, ret, ...)`, from `include/exec/helper-adapter.h`,
which only expands to a real function body (instead of a no-op) if
`GEN_ADAPTER_DEFINE` was `#define`d *before* `helper-adapter.h` is pulled
in. Across the entire `deps/unicorn` tree, exactly one file does this:
`target/i386/translate.c` (`#define GEN_ADAPTER_DEFINE` immediately before
its `osdep.h`/`helper-proto.h`/`helper-gen.h` includes). No other target has
an equivalent — meaning none of them have ever actually linked.

**Why the fix is correct:** added the identical `#define GEN_ADAPTER_DEFINE`
line to `target/arm/translate.c`, in the same position relative to its
includes, mirroring i386's pattern exactly. `target/arm/helper.h` is the
single unified helper list for the whole target — it `#include`s
`helper-a64.h` and `helper-sve.h` itself — so generating from `translate.c`
alone covers both AArch32 and AArch64 instructions.
`target/arm/translate-a64.c` and `translate-sve.c` also include
`helper-gen.h` but don't set `GEN_ADAPTER_DEFINE`, so they correctly stay
silent and don't produce duplicate definitions within the same target.

**x86 regression evidence:** none possible — the file changed
(`target/arm/translate.c`) is never compiled into `x86_64-softmmu`.

### 4. Adapter thunks escaped the per-target symbol renaming

Carried separately as `cmake/unicorn-patches/0002-per-target-adapter-helper-names.patch`.

**Symptom:** 246 distinct `ld: warning: duplicate symbol '_adapter_helper_*'`
warnings between `libx86_64-softmmu.a` and `libaarch64-softmmu.a` (~490 log
lines). Apple's `ld` only warns, so the build "succeeded"; `ld.lld` rejects
the same shape outright with `duplicate symbol` unless
`--allow-multiple-definition` is passed, so the Linux CI legs would have
failed at link time.

**Root cause — and it is a correctness bug, not just noise.** Every QEMU
symbol in this fork is given a per-target name: each softmmu target is
compiled with `-include <arch>.h` (`deps/unicorn/CMakeLists.txt`), and that
autogenerated header both defines `UNICORN_ARCH_POSTFIX` (`_aarch64`,
`_x86_64`, …) and remaps names, e.g.
`#define helper_atomic_add_fetchb helper_atomic_add_fetchb_aarch64`.

The adapter thunks never got that treatment. `helper-adapter.h`,
`helper-proto.h`, `helper-gen.h` and `helper-tcg.h` all spelled the name as
`glue(adapter_helper_, name)` — no postfix. But a thunk's *body* calls
`HELPER(name)`, which **is** remapped. So each target emitted a thunk with an
identical name and a different body, and the linker silently kept one:
`x86_64`'s TCG could end up calling through `aarch64`'s thunk into
`helper_..._aarch64` with an x86 `CPUArchState`.

The duplication was previously characterised as harmless because the bodies
were assumed to be content-identical trivial integer ops. **That was wrong,
and it was wrong for all 246 names — not merely for the 233 outside the
`GLOB_NAME_` list.** Of the 13 `GLOB_NAME_` entries, only `uc_tracecode` is
genuinely un-remapped (it appears in no `qemu/<arch>.h` at all). The other
12 — `div_i32` and friends — are per-target like everything else
(`x86_64.h:1106` → `helper_div_i32_x86_64`,
`aarch64.h:1106` → `helper_div_i32_aarch64`), so their thunk bodies differ
*textually* and are only *semantically* equivalent. Not one of the 246
duplicated thunks was ever safe to collapse onto a single definition; the 12
would merely have failed less visibly than the rest.

**Why the fix is correct:** it copies an already-established, already-working
pattern from the same codebase. `memory.h:930` and
`memory_ldst_cached.inc.h:20` already guard on `#ifdef UNICORN_ARCH_POSTFIX`
and append it via `glue()` to give `address_space_*` per-target names. The
patch adds one `ADAPTER_HELPER(name)` macro to `helper-head.h` — beside the
existing `HELPER(name)`, the header all four consumers already include — and
routes all 25 naming sites through it. No per-name list to maintain, and the
`#else` branch leaves non-Unicorn builds untouched.

**Rejected alternative:** re-enabling the disabled deduplication
(`#define IS_GLOB(name) 0 // CHECK(GLOB_PROBE(name))`) covers only 13 of the
246 duplicated names, and because nothing outside `translate.c` defines
`GEN_ADAPTER_DEFINE`, enabling it would leave those 13 undefined at link
time. It addresses 5% of the problem and breaks the link doing so. Marking
the thunks `weak` was also rejected: it would collapse *differently-bodied*
functions onto one definition, making the silent mis-dispatch above
permanent rather than fixing it.

**Regression evidence:** `linux-emulator-test` 38/38 pass — real x86-64 TCG
execution (ELF loading, syscalls, mmap/brk, openat/getdents) through the
exact machinery this patch changes. `arm64-backend-test` 25/25 pass. All 246
duplicate-symbol warnings are gone, and `nm` confirms the thunks are now
`_adapter_helper_atomic_add_fetchb_x86_64` /
`..._aarch64`. Verified under emscripten too: `wasm-ld` links a real
executable that extracts *both* targets' `translate.c.o` (confirmed with
`--why-extract`) with zero duplicate-symbol errors and no
`--allow-multiple-definition`.

### 5. `LDRAA`/`LDRAB` authenticated with SP instead of a zero modifier

Carried separately as `cmake/unicorn-patches/0003-ldra-zero-modifier.patch`.

**Symptom:** every C++ virtual call in an arm64e guest faults. `EXC_BAD_ACCESS`
in `__gxx_personality_v0+0x528` killed the emulated Calculator at ~190M
instructions, and a twenty-line arm64e program that throws and catches
`std::runtime_error` reproduces it in 4.8M.

**Root cause:** `disas_ldst_pac` (`qemu/target/arm/translate-a64.c:3407-3413`)
passed `tcg_ctx->cpu_X[31]` — which is SP, `regnames[31] == "sp"` — as the
modifier to `autda`/`autdb`. The A64 definition of LDRAA/LDRAB is "authenticates
an address from a base register using a **modifier of zero** and the specified
key". Every other PAC site in the file is right: the `*SP` forms pass
`cpu_X[31]`, the `*Z` forms pass `new_tmp_a64_zero(s)`, and the register forms
pass `cpu_reg_sp(s, rn)`.

The instruction only appears in Apple code, which is why an x86 fork could carry
this indefinitely: clang emits it for the arm64e C++ vtable dispatch, where the
vtable pointer inside an object is signed with key DA and a zero discriminator.
Nothing in Linux/glibc AArch64 emits LDRAA at all.

**Measured on this host (25G76), not inferred.** an arm64e binary that runs `pacdza x8`
followed by `ldraa x9, [x8, #0x20]` loads through the signature with SP holding a
live stack address. The same shape is pinned in the emulator as
`Arm64Backend.LoadWithPointerAuthenticationUsesAZeroModifier`. Independently,
the shared cache's own auth-rebase entry for
`libc++abi.dylib __AUTH_CONST` + 10776 — the vtable slot of
`typeinfo for std::exception` — records `keyIsData=1, addrDiv=0, diversity=0`,
so the kernel signs it with modifier 0 and the guest's LDRAA authenticates it.

**x86 regression evidence:** none possible — `translate-a64.c` is compiled only
into `aarch64-softmmu`.

## Known residual: `cpu_interrupt_handler` is defined in both archives

`nm -g --defined-only` still reports one symbol defined in both
`libx86_64-softmmu.a` and `libaarch64-softmmu.a`:
`_cpu_interrupt_handler` (type `D`), from `tcg-all.c.o` in each. It is
pre-existing, unrelated to the adapter thunks, and **benign** — recorded here
so the next person who runs `nm` does not have to re-derive that.

`tcg-all.c.o` defines **exactly one** global symbol, that one. Under standard
archive semantics a member is extracted only to satisfy an outstanding
undefined reference, so whichever archive resolves `cpu_interrupt_handler`
first supplies it and the other archive's `tcg-all.c.o` is never pulled in.
No duplicate definition ever reaches the linker. This is shared behaviour
between `ld.bfd` and `lld`, and bfd's single-pass archive ordering can only
extract *fewer* members than lld, never more. Nothing in the build uses
`--whole-archive`, which is the one thing that would force both in.

Contrast the adapter thunks, which genuinely did collide: their defining
member `translate.c.o` carries 790 (x86_64) / 1848 (aarch64) other global
definitions and many undefined references, so *both* copies were always
force-extracted.

## Build-environment consequences

Carrying the fixes as configure-time patches costs two things that did not
previously constrain a sogen build:

- **git is now a hard build dependency, and the source tree must be writable.**
  `deps/CMakeLists.txt:1` calls `find_package(Git REQUIRED)` and the apply step
  runs `git apply` *into* `deps/unicorn`. A checkout without git on `PATH` fails
  at configure time, and so does any read-only source tree — a packaged tarball,
  a shared or container-mounted checkout, a distro build sandbox. That is a new
  failure class, not a stricter version of an existing one: nothing else in the
  build writes into the source tree.
- **A stale build directory fails at runtime, not at configure time.**
  `deps/CMakeLists.txt:45` sets `UNICORN_ARCH "x86;aarch64" CACHE STRING ""`
  with no `FORCE`, so a build tree configured before this branch keeps its cached
  `x86` value. Nothing warns: the configure succeeds, `libaarch64-softmmu.a` is
  never built, and the first symptom is `uc_open(UC_ARCH_ARM64, ...)` returning
  `UC_ERR_ARCH` at runtime. **Configure a fresh build directory** when moving
  onto this branch, or clear `UNICORN_ARCH` from the existing cache.

## Maintaining these patches

If the `deps/unicorn` submodule pin moves, either patch may stop applying.
`deps/CMakeLists.txt` fails the configure loudly in that case (`git apply`
fails and the tree isn't already patched) rather than silently building an
aarch64 target that doesn't actually work. Before regenerating, check whether
momo5502/unicorn has picked up an upstream fix and drop the affected patch
entirely.

**Regenerate each patch with a scoped path filter — never a bare tree diff.**
The three patches are disjoint by file, and that is what keeps them separable:

| Patch | Files |
| --- | --- |
| `0001-enable-aarch64-build.patch` | `qemu/exec.c`, `qemu/target/arm/translate.c` |
| `0002-per-target-adapter-helper-names.patch` | `qemu/include/exec/helper-{head,adapter,proto,gen,tcg}.h` |
| `0003-ldra-zero-modifier.patch` | `qemu/target/arm/translate-a64.c` |

After re-applying the equivalent fixes to the new pin, regenerate from the
repository root:

```sh
git -C deps/unicorn diff -- \
  qemu/exec.c qemu/target/arm/translate.c \
  > cmake/unicorn-patches/0001-enable-aarch64-build.patch

git -C deps/unicorn diff -- \
  qemu/include/exec/helper-head.h \
  qemu/include/exec/helper-adapter.h \
  qemu/include/exec/helper-proto.h \
  qemu/include/exec/helper-gen.h \
  qemu/include/exec/helper-tcg.h \
  > cmake/unicorn-patches/0002-per-target-adapter-helper-names.patch

git -C deps/unicorn diff -- \
  qemu/target/arm/translate-a64.c \
  > cmake/unicorn-patches/0003-ldra-zero-modifier.patch
```

A bare `git -C deps/unicorn diff > …/0001-….patch` would fold **both** patches'
hunks into `0001`. `0002` would then either double-apply or fail forever,
because the apply step's "already applied?" probe (`git apply --reverse
--check`) would see its hunks already present via `0001`. Scope every
regeneration.

Verify afterwards that each patch still applies from a pristine tree and that
the step stays idempotent:

```sh
git -C deps/unicorn checkout -- .
cmake --preset=release   # applies all three, in order
cmake --preset=release   # second run must be a no-op
git -C deps/unicorn diff --name-only   # expect exactly the 8 files above
```
