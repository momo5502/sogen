# Mach-O loader — development notes

Stage 2 of the macOS arm64 emulation programme: enough of a Mach-O loader to map the guest's main
executable and `/usr/lib/dyld` into an AArch64 guest address space and start executing dyld's entry
point. It sits on the arm64 Unicorn backend from Stage 1 (`docs/arm64-backend.md`).

This document records what was *built* and what was deliberately *not* built, together with the
measured Mach-O and shared-cache facts that forced each decision.

## Status

`macos-emulator-test` passes 114/114 on macOS arm64. Six of those tests are host-gated and
`GTEST_SKIP()` rather than fail when their input is absent, so the suite is green on Linux, Windows
and emscripten too:

| Gate | Tests |
| --- | --- |
| `/usr/lib/dyld` exists | `MacosModuleManager.MapsTheRealHostDyldAndExecutesItsFirstInstructions` |
| `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e` exists | `DyldCacheFormat.ReadsTheRealHostCacheHeader`, `DyldCacheFormat.DecodesSlideInfoInAHostSubcache`, `DyldSharedCacheReader.ReadsTheRealHostCache`, `…MakesNoStructuralAssumptionsAboutSubcaches`, `…TouchesOnlyMetadataOfTheRealCache` |

Everything else runs from the four committed self-built fixtures on every platform.

The paths those two gates name are set from `src/macos-emulator-test/CMakeLists.txt`
(`MACOS_DYLD_HOST_PATH`, `MACOS_DYLD_CACHE_HOST_PATH`), not hardcoded in the tests.

## Architecture

- `src/emulator-platform/platform/macho.hpp` — the format layer, mirroring `platform/elf.hpp`. Mach-O
  headers, fat/universal slices, load-command walking, and the `dyld_cache_*` on-disk structures with
  `static_assert`ed sizes. Header-only, no guest memory, no I/O.
- `src/macos-emulator/macos_memory_manager.{hpp,cpp}` — the Darwin guest address space. Publicly
  exposes `allocate_memory` / `reserve_memory` / `protect_memory` / `release_memory` /
  `map_host_file_memory`; the `memory_interface` address-space virtuals stay private.
- `src/macos-emulator/module/macho_mapping.{hpp,cpp}` — slice selection, metadata extraction
  (`read_macho_module_metadata`) and mapping (`map_macho_from_data`).
- `src/macos-emulator/module/macos_module_manager.{hpp,cpp}` — reads images from disk, resolves guest
  paths through an emulation root, and owns the `image_base → macos_mapped_module` map.
- `src/macos-emulator/module/dyld_shared_cache.{hpp,cpp}` — a read-only cache introspection reader.
  **Off the load path**; it has no consumer until Stage 6 wants symbolization.
- `src/macos-emulator-test/` — the gtest suite and the fixtures.

`src/emulator/memory_interface.hpp` gained exactly one friend declaration (`macos_memory_manager`)
alongside `memory_manager` and `linux_memory_manager`. `linux-emulator-test` (38/38) and
`arm64-backend-test` (25/25) are the regression check for that header.

## Scope: sogen maps, dyld links

**Sogen maps exactly two images — the main executable and `/usr/lib/dyld` — and applies no fixups, no
relocations, no symbol binding and no cache mapping.** This is the single biggest scope lever in the
programme and it is settled, not provisional. Do not reopen it.

The proof is dyld rebasing *its own* `__DATA_CONST` with no help from us, read from the file
and then from a live private-mode process:

```
file   /usr/lib/dyld arm64e __DATA_CONST[2] = 00200000000a123e   (fmt-12 rebase, target 0xa123e)
live   (private, dyld @ 0x105270000)        = 000000010531123e   = 0x105270000 + 0xa123e   ✔
```

`_dyld_start` self-rebases, then rebases the cache, then rebases and binds the main executable. The
work sogen would otherwise own — 26.9 M slide-v5 pointers unpacked, 9.7 M of them PAC-signed, all 34
cache mappings, every `LC_LOAD_DYLIB` resolution — is dyld's.

`macho.hpp` *decodes* chained-fixup and slide-info pointers, and `macho_fixups_test.cpp` pins that
decoding against the real fixup table of a committed fixture. That is for analysis and for the
cache reader. Nothing in this stage ever writes a decoded pointer back into guest memory.

### `DYLD_SHARED_REGION=private` is mandatory for Stage 3

Stage 3 must put `DYLD_SHARED_REGION=private` in the guest environment. Two consequences if it does
not:

1. dyld calls `shared_region_check_np` (syscall 294) and takes the system-wide path.
2. On that path control is handed to the copy of dyld living *inside* the cache
   (`dyldInCacheMH 0x180114000` + slide), not to the on-disk image sogen mapped. Sogen would then have
   to implement the re-exec-into-cache handoff.

Private mode keeps execution inside the image we mapped. That is why `MACOS_DYLD_DEFAULT_BASE` is
`0x105270000` — the address a real private-mode launch on build 25G76 chose, so emulator traces line
up with host traces.

## `image_base` vs `image_start` — read this before touching `contains()`

`macos_mapped_module` carries both, and they are not interchangeable:

| Field | Meaning |
| --- | --- |
| `preferred_base` | the mach header's `vmaddr` as the file wants it (`mach_header_vmaddr`, which skips `__PAGEZERO`) |
| `image_base` | where the mach header actually lands after any slide |
| `image_start` | `vaddr_min` — the page-aligned-down lowest mapped segment |
| `size_of_image` | `vaddr_max - vaddr_min`, both ends page-aligned |

`contains()` measures from `image_start`, **not** from `image_base`:

```cpp
return (address - this->image_start) < this->size_of_image;
```

`image_base` must stay the mach header, because two things are defined relative to it and nothing
else: `LC_MAIN`'s `entryoff`, and every chained-fixup target. Redefining `image_base` as the lowest
mapped address — which looks like a tidy-up — silently moves the entry point and every fixup target
of any image whose header is not its lowest segment.

Equally, measuring containment from `image_base` runs the window past the end of the last segment
whenever a segment sits below the header. `MachoMetadata.MeasuresContainmentFromTheLowestSegmentNot
FromTheMachHeader` fails if either half of this is changed, and `macos_module_manager::find_by_address`
scans linearly rather than doing an ordered lookup on the map key for exactly this reason: the map is
keyed by `image_base` while the extent starts at `image_start`.

`LC_UNIXTHREAD` and `LC_MAIN` are also derived differently and must stay that way — `LC_UNIXTHREAD`
carries an unslid VA (`state->pc + slide`), `LC_MAIN` an offset from the mach header
(`image_base + entryoff`). Deriving both the same way is the classic mistake.

## Everything is bounded by the slice, never by the buffer

Every parse in `macho.hpp` and `macho_mapping.cpp` bounds against the **slice extent**, obtained from
`get_slice(data, slice_offset)`, not against the whole file buffer.

The threat is specific: in a fat binary, a segment's `fileoff`/`filesize` can point into a *different*
slice. Bounded against the whole buffer that is perfectly in range, and sogen would map slice B's
bytes while reporting slice A's header, architecture and entry point. `fileoff` is bounded even when
`filesize` is 0, because `slice_offset + fileoff` is still recorded and would otherwise be free to
wrap.

**A thin fixture cannot test this.** In a thin image the slice *is* the whole buffer, so the
slice-bounded and buffer-bounded checks are identical and the original test was structurally incapable
of failing. Only a fat fixture discriminates. The tests that actually hold the property are
`MachoMetadata.RejectsASegmentThatEscapesItsSliceButNotTheFatBuffer`,
`MachoMetadata.RejectsAFileOffsetOutsideTheSliceEvenWhenNothingIsMappedFromIt`,
`MachoFat.RejectsASliceWhoseCommandTableReachesIntoTheNextSlice` and
`MachoFat.SelectedSliceHeaderMatchesTheSliceItNamed` — all four on `macho_fat_arm64_arm64e`. If you
add a bounds check, add its test on the fat fixture or you have added nothing.

Slice selection prefers arm64e over arm64 and matches on `cpusubtype & ~CPU_SUBTYPE_MASK`: the on-disk
arm64e subtype is `0x80000002` (`CPU_SUBTYPE_ARM64E | CPU_SUBTYPE_PTRAUTH_ABI`), so an equality
comparison against `CPU_SUBTYPE_ARM64E` finds nothing. Fat header fields are big-endian.

## The mapper has no rollback, and the manager must not pretend otherwise

`map_macho_from_data` maps segments in a loop. A throw part-way through leaves every earlier segment
mapped in the guest. There is no unwind.

`macos_module_manager::map_module` therefore **propagates** the exception. Copying the Windows module
manager's catch-and-return-`nullptr` shape here would be a real bug, not a style difference:

- the failed segments stay in `macos_memory_manager::mapped_regions_`, leaked;
- `overlaps_mapped_region` then rejects any retry at the same base, so the caller's "it returned
  null, try again" recovery can never succeed;
- and a caller cannot distinguish a half-occupied address space from an empty one.

`MacosModuleManager.PropagatesAMidMappingFailureAndRecordsNothing` pins this: it pre-occupies
`__LINKEDIT`'s base, expects the throw, and then asserts both that the manager recorded nothing *and*
that the earlier segment is still mapped.

**Any future stage that needs to retry a mapping must make `map_macho_from_data` transactional
first** — track the segments mapped so far and `release_memory` them on the way out. Do not instead
soften the manager.

## Page size and `__PAGEZERO`

The guest page size is 16 KiB, `MACOS_PAGE_SIZE = 0x4000`, defined once in `macos_memory_manager.hpp`.

**`page_align_up` and `page_align_down` default to `0x1000`** (`src/emulator/address_utils.hpp`). A
call that omits the second argument compiles, runs, and quietly aligns to the wrong page size. Pass
`MACOS_PAGE_SIZE` explicitly at every call site. There is no compiler diagnostic for getting this
wrong.

Because arm64 Mach-O `vmaddr`/`vmsize` are 16 KiB aligned, two segments never share a page, so there is
no ELF-style shared-page fixup. Segments are still mapped read-write and protected afterwards, because
`__TEXT` and `__LINKEDIT` are unwritable in their final state and the file bytes have to be written
first.

`__PAGEZERO` is **reserved, never mapped** — `reserve_memory` records the range in `mapped_regions_`
with `backed = false` and calls no `map_memory`. Two independent reasons:

1. `uc_mem_map` allocates host RAM eagerly. A 4 GiB `__PAGEZERO` would commit 4 GiB of host RAM.
2. A mapped `__PAGEZERO` makes guest null dereferences *succeed*, destroying the fault that a
   malware-analysis emulator most wants to see.

`MachoLoader.PageZeroIsReservedButNeverBacked` and
`MacosMemoryManager.ReservedRangesAreClaimedButNeverBacked` hold this — but see the fault-visibility
warning below, which is about to make both of them unable to fail.

## Two structure layouts that are easy to get wrong

Both were verified against live data. Code from the wrong layout and neither works.

**1. `DYLD_CHAINED_PTR_64` / `_64_OFFSET` are not laid out like `arm64e`.** `next` is **12 bits
(51–62)** and `bind` is **bit 63** — not 11 bits with `bind` at 62, which is the *arm64e* layout. Per
`dyld_chained_ptr_64_rebase` / `_bind` in `<mach-o/fixup-chains.h>`. A pointer of
`0x8000000000000000` (`bind libSystem/_printf`) settles it: that value has only bit 63 set, so bit 63
is where `bind` lives. `decode_64_pointer` implements this layout and
`MachoFixups.Format64NextSpansBit62` pins it.

**2. `dyld_cache_image_text_info` is 32 bytes, not 40.** `uuid[16]` + `load_address` (8)
+ `text_segment_size` (4) + `path_offset` (4). At 40 the image-text walk finds nothing at all — every
entry decodes to garbage and the loop silently produces an empty list. At 32, entries 0–2 decode to
`/usr/lib/libobjc.A.dylib`, `/usr/lib/system/libdyld.dylib` and `/usr/lib/dyld`, the last matching the
header's `dyldInCacheMH` exactly. `static_assert(sizeof(dyld_cache_image_text_info) == 32)` and
`DyldCacheFormat.ReadsTheRealHostCacheHeader` (which asserts an entry matched `dyldInCacheMH`, with
the failure message naming the entry size) both hold it.

## The shared cache's real structure

Measured on macOS 26.6.1 build 25G76, arm64e:

| | |
| --- | --- |
| Base file | **573 440 bytes** (~560 KiB) |
| `mappingCount` / `mappingWithSlideCount` in the base file | **1** / 1 — a single `r-x` mapping of `0x88000` at `0x180000000` |
| Subcaches | **12**, listed in the base file's subcache array |
| Files / regions total | 13 / **34** |
| `sharedRegionStart` / `sharedRegionSize` | `0x180000000` / `0x166444000` |
| `maxSlide` | `0x10000000` |
| Images | **3649** |
| `dyldInCacheMH` | `0x180114000` → `/usr/lib/dyld` |
| `formatFlags` | `0x1000` — `dylibs_expected_on_disk` is **false** |

**A reader that consults only the base file sees essentially nothing.** All real mapping, slide and
protection structure lives in the 12 subcache headers. `dyld_shared_cache_reader` therefore opens each
subcache and reads its header and mapping array, and verifies each subcache header's UUID against the
base file's `dyld_subcache_entry`.

It makes **no structural assumptions** about the subcaches, because none hold. Mapping counts per
file are 1, 1, 7, 2, 2, 1, 6, 2, 2, 1, 6, 1, 2 — five files carry a single mapping, `.11` is a 32 KiB
file with one 16 KiB `r-x` mapping, and only `.02`, `.06` and `.10` carry slide info, leaving 10 files
with none. `DyldSharedCacheReader.MakesNoStructuralAssumptionsAboutSubcaches` pins both of those.

The reader never reads a whole file: `.01` alone is 1.63 GiB and the set is 5.42 GiB, yet the reader
touches on the order of **0.068%** of it. It issues one 0x400-byte header read, one mapping-array read,
and for the base file one subcache array and the image arrays.
`DyldSharedCacheReader.TouchesOnlyMetadataOfTheRealCache` pins that its total reads stay under 8 MiB.
Subcaches are never parsed recursively, so there is no cycle.

## `is_arm64e_chained_format` is a guard, not a convenience

`decode_arm64e_pointer` handles the five arm64e formats: 1 (`ARM64E`), 7 (`ARM64E_KERNEL`), 9
(`ARM64E_USERLAND`), 10 (`ARM64E_FIRMWARE`) and 12 (`ARM64E_USERLAND24`). Handed format **13**
(`ARM64E_SHARED_CACHE`) or **14** (`ARM64E_SEGMENTED`) it does not fail — it decodes the wrong
bitfields and returns a structurally valid answer. Callers must gate on
`is_arm64e_chained_format(fmt)` first; format 13 belongs to the slide-info-v5 decoder, and 14 has no
decoder at all.

Returning a zeroed struct on an unknown format would **not** have been a loud failure: an all-zero
decode is a well-formed terminal rebase to offset 0 (`auth=0, bind=0, next=0, target=0`), which is
indistinguishable from a legitimate chain end. There is no in-band way to signal "wrong format" from
this decoder, which is why the gate is a separate predicate.
`MachoFixups.IdentifiesTheArm64eFormatsTheDecoderHandles` enumerates exactly which formats the gate
admits.

## Fault visibility — a dependency Stage 3 will break

This stage registers **no** emulator hooks. Guest faults therefore surface as `UC_ERR_*` and
`try_read_memory` reports them, which is what makes
`MachoLoader.PageZeroIsReservedButNeverBacked` and
`MacosMemoryManager.ReservedRangesAreClaimedButNeverBacked` able to fail at all: they assert that a
guest access to a reserved-but-unbacked range *does not* succeed.

The moment Stage 3 registers its `svc` hook, Unicorn's `UC_HOOK_INTR` fault suppression
(`docs/arm64-backend.md`, "Fault suppression") swallows every exception index below `EXCP_INTERRUPT`,
including data aborts the hook explicitly filters out. Both tests would then pass unconditionally and
stop protecting anything.

**Stage 3 must register a catch-all interrupt hook that halts the CPU on any unexpected exception
index, and re-establish this property before relying on either test.** This is not optional cleanup;
it is the load-bearing half of `__PAGEZERO` being useful.

## What Stage 3 inherits

- **The 106-syscall ceiling** — a hard cap measured from dyld's own `svc` stubs. The subset the
  private-cache path needs is `mmap`, `mprotect`, `open`, `openat`, `fstat64`, `munmap`, `madvise`,
  `close`. ABI: `svc #0x80`, number in `x16`, Mach traps negative, error signalled by
  carry-**set**.
- **`DYLD_SHARED_REGION=private` in the guest environment**, per above.
- **`map_host_file_memory` is the native backing route**, and its aliasing behaviour is now tested
  (`MacosMemoryManager.HostFileMappingIsCopyOnWrite`).

  **Unicorn provides no copy-on-write of its own.** `uc_mem_map_ptr` sets `RAM_PREALLOC` and aliases
  the caller's host buffer directly: guest writes land in host memory immediately and are visible at
  that pointer. Whatever sharing semantics the *host* `mmap` had are exactly what the guest gets.

  **So Stage 3's `mmap` must pass `MAP_PRIVATE`.** A `MAP_SHARED` host mapping would write through to
  the user's real macOS system files — the cache, `/usr/lib/dyld`, anything the guest opens.

  Two corollaries: ownership stays with the caller (neither `uc_mem_unmap` nor `uc_close` frees the
  pointer, so Stage 3 must `munmap` it itself), and ptr-mapped regions use
  `memory_region_add_subregion` rather than the `_overlap` variant, so they are **not** snapshot-aware.
  Irrelevant here, but it matters the first time native `mmap` meets Unicorn context snapshots.
- **`macos_memory_manager` has no serialization** and no allocate/protect/release callbacks. Stage 3
  adds them when the syscall layer needs them.

## What Stage 8 inherits

- **wasm32 cannot host the cache at all.** Linear memory is capped at 4 GiB by the ISA; the cache's
  mappings need ~5.4 GiB of backing bytes. No configuration fixes this.
- **wasm64 fits under its 8 GiB maximum, but nothing else does.** There is no `mmap` under emscripten
  — `qemu_ram_mmap` degenerates to an in-linear-memory allocation, so every cache byte must be
  transferred to the browser and held resident.
- The only viable shape is a **demand-fault page provider** (`UC_HOOK_MEM_UNMAPPED` or MMIO over the
  cache VA window, materializing 16 KiB pages on first touch). The measured private-mode working set
  is ~6 MiB of text plus ~1 MiB of dirtied data — on the order of 10–20 MiB fetched, not 5.4 GiB. This
  is a paging layer interacting with `-sASYNCIFY`, **not a preset flag**.

## Fixtures

Four committed, self-built Mach-O images in `src/macos-emulator-test/fixtures/`. They are committed
rather than generated at build time so the suite runs on Linux, Windows and emscripten.

| Fixture | Bytes | SHA-256 |
| --- | --- | --- |
| `macho_static_arm64` | 16 448 | `99cfd6c01656ec8f4ebb151dd6d6996001bdf6cc45ee9c1c5b068c43add12fc0` |
| `macho_static_arm64e` | 16 448 | `cf8c8ecc2b9045dd7c96e3265646c94c5a56d7cf83f221d837fbbf5662621d3c` |
| `macho_fat_arm64_arm64e` | 65 600 | `2b4d0315008c52c60e6f661544b25b290fac7d1105d5d67a73ae7d4072a7304e` |
| `macho_dylink_arm64` | 33 440 | `2110c60e09d75c8cef5f73581788a935c5f982808c27a98ed6b67798ad3379d0` |

Regenerate with `src/macos-emulator-test/fixtures/build-fixtures.sh` (macOS plus the Xcode command
line tools). `-Wl,-no_uuid` is what makes the output reproducible.

**The output basename is load-bearing.** The linker's ad-hoc code signature hashes it, so renaming a
fixture changes its bytes and its SHA-256. Rename only by editing the script and re-recording the
hashes here.

The two static slices differ deliberately in the value they leave in `x0` — `0x5e` under
`__arm64e__`, `0x2a` otherwise — which is what lets
`MachoLoader.MapsTheArm64eSliceOfTheFatFixtureAndExecutesIt` fail with a *specific* wrong value if
slice selection ever picks the first fat entry instead of the arm64e one.

## Build & test

```sh
cmake --build --preset=release --target macos-emulator-test
cd build/release/artifacts && ./macos-emulator-test
```

`macos-emulator-test` is registered with CTest (`add_test` in
`src/macos-emulator-test/CMakeLists.txt`) and `src/CMakeLists.txt` adds it unconditionally, so it runs
on every platform CI tests. `.github/workflows/ci-reusable.yml` also invokes it explicitly, alongside
`arm64-backend-test`, ahead of the emulation-root download that neither test needs — a regression in
either stays distinguishable from a failure in the much heavier root setup.

### clang-tidy

Both `macos-emulator` and `macos-emulator-test` are in `OWN_TARGETS` (they come in through
`sogen_add_subdirectory_and_get_targets("src" …)`), so both are checked — unlike `unicorn-emulator`,
which is not. The caveats in `docs/arm64-backend.md` apply verbatim: CI pins LLVM 21, a newer local
clang-tidy reports checks that do not exist there, and `find_program` silently skips clang-tidy
entirely if it is not on `PATH`. Pass `-DCLANG_TIDY_EXECUTABLE=` explicitly to be certain it ran.

```sh
cmake --build --preset=tidy
```
