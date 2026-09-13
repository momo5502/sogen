# macOS memory

The guest address-space layout, the guest/emulator split, and the GUI arena's lifetime rule — the layer
[concurrency](macos-concurrency.md) and the [window server](macos-window-server.md) both allocate
through without owning themselves. This document exists because the single most instructive bug in the
macOS programme so far was a memory-layout accident, not a logic error, and the accident is worth
understanding on its own terms.

## Status

The GUI arena and the guest's mmap arena no longer overlap. `ensure_backing_store` resizes a window's
backing store on resize rather than trusting a stale allocation. Ten of ten runs of the finished change
are clean at the same pixel counts as the pre-existing baseline. A second, unidentified source of intermittent guest-heap corruption remains open — see "Still open" below.

## The address-space map

Every named range below is a `constexpr` in `src/macos-emulator/macos_platform.hpp`.

| Range | Base | Size | What |
| --- | --- | --- | --- |
| shared cache | `0x180000000` | to `0x2E6440000` | the mapped dyld shared cache |
| GUI trap page | `0x2F0000000` | one page | the continuation trap the guest-call stack resumes on |
| GUI arena | `0x300000000` | 1 GiB | window backing stores, layer-contents rasters, CF bridge scratch, IOSurface backing, window-server OOL replies |
| workqueue arena | `0x340000000` | 64 MiB | one stack + one pthread-struct page per spawned worker, at a fixed per-worker offset |
| guest mmap arena | `0x300000000` *(before the fix below)* | — | where the guest's own `mmap`/`malloc` allocations land when nothing else names an address |
| main thread stack | up to `0x16FC00000` | — | the main thread's own stack |
| commpage nesting | `0xFC0000000` | 1 GiB | the two-page Darwin commpage |

## The guest/emulator split, and why released ranges are withheld

`MACOS_GUI_ARENA_BASE` and `MACOS_DEFAULT_MMAP_BASE` were both `0x300000000` — the same address. sogen's
"GUI arena" **was** the guest's mmap arena: every emulator-owned guest buffer the window path allocates
was carved out of the exact pool the guest's own `malloc`/`mmap` draws from when it wants an address
sogen has not pinned elsewhere.

The GUI arena is not a reservation in the sense its name suggests. It is a *hint* passed to
`macos_memory_manager::allocate_memory`, and `find_free_allocation_base` treats a hint as a floor and
then walks forward past every already-mapped region without ever stopping at the arena's declared end.
So an emulator-owned block released back to the memory manager becomes, from the guest allocator's point
of view, ordinary free memory in exactly the range the guest already searches — and the guest's own
allocator can and does take it.

**The fix is not to move the arena; it is to move the guest.** Giving the GUI arena an address of its
own does not give it a *range* of its own, because the walk that finds free memory does not stop at an
arbitrary boundary — it was tried, at three different addresses, and it clears the fault ten runs out of
ten, but Calculator then composes an **empty** window, because something else in the window path
silently depended on the overlap (see below). The change that actually separates the two arenas without
losing the window is `macos_gui_arena::exclude_guest`
(`src/macos-emulator/gui/macos_ui_state.cpp`), called from `macos_ui_state::bind` while dyld is still
opening the shared cache — early enough that the arena is still empty:

```cpp
void macos_gui_arena::exclude_guest(macos_emulator& emu)
{
    constexpr auto arenas_end =
        std::max(MACOS_GUI_ARENA_BASE + MACOS_GUI_ARENA_SIZE, MACOS_WORKQUEUE_ARENA_BASE + MACOS_WORKQUEUE_ARENA_SIZE);
    emu.memory.set_mmap_base(std::max(emu.memory.get_mmap_base(), arenas_end));
}
```

This raises the guest's own unhinted-allocation floor past both the GUI arena and the workqueue arena
(the workqueue arena is included because its slots are claimed at *fixed* addresses, and a guest
allocation reaching one first would make worker creation fail). The floor only ever moves forward:
whatever the guest already owns below it stays the guest's, and no future unhinted guest allocation can
land inside either arena again. What a *hinted* allocation can still do is listed under "Still open".

## The use-after-free this closes, and the evidence

With the arenas overlapping, sogen's own memory-write watch — instrumented to catch every
emulator-originated write and never fired once during the failing runs — proved sogen itself was not the
writer. A history of every map/unmap the memory manager performed named the actual sequence: ten
`map 0x4000`/`unmap 0x4000` cycles at one address (a CoreFoundation-bridge scratch buffer allocated and
released over and over, from `gui/macos_cf_bridge.cpp`'s `release_scratch`), and then a `map 0x8000` at
the same address — the guest's own allocator taking over a range the emulator had just released.

The corrupted per-thread malloc cache dumps as a repeating `1e 1e 1e ff` — which is **BGRA for
RGB(30,30,30)**, Calculator's window background colour. The guest was
painting its own window background directly over its own malloc heap, because the emulator had handed
that address to the window path and then, independently, released it back into the pool the guest's
allocator draws from.

## The bug the overlap was hiding

Once the arenas are separated, the fault the malloc-cache measurement caught goes away — and Calculator's
window goes blank instead, which is the more instructive half. The real defect is in
`macos_ui_state::ensure_backing_store` (`src/macos-emulator/gui/macos_ui_state.cpp`): a window keeps its
backing-store record across a resize, but the record's `backing_stride` was fixed at the width the window
had when the store was **first** allocated, while every reader sizes itself from
`backing_stride * height`, with `height` read live off the current record.

Calculator opens at 460×52 and settles at 230×408. The store is allocated for the first extent: stride
`460 * 4 = 1840` bytes, `1840 * 52 = 95,680` bytes, rounded up to six 16 KiB pages = **98,304 bytes**. By
the fourth composite the window record says `height = 408`, so `backing_bytes()` computes
`1840 * 408 = 750,720` bytes — and `macos_layer_tree_present` writes all 750,720 of them at
`backing_address`, **40 pages past the end of a 98,304-byte allocation**.

Under the overlap, those 40 pages happened to be mapped — they were the guest's own heap, sitting
immediately above a block the emulator had carved out of the same pool — so the out-of-bounds write and
the out-of-bounds read-back both silently succeeded and the window looked correct. Give the arena its
own range and those 40 pages are unmapped: both calls fail, the present is skipped, and Calculator
composes a frame with nothing in it. The fix makes `ensure_backing_store` re-check the store's capacity
against the window's *current* size and reallocate when it no longer fits, rather than trusting
`backing_address != 0` as proof the store is still big enough.

## The lifetime rule

sogen hands the guest raw pointers into arena blocks — `SLWindowContextCreate` builds a `CGBitmapContext`
directly over a window's backing store, `macos_layer_contents` builds one over a raster, the
CoreFoundation bridge passes scratch offsets into `CFStringCreateWithBytes` — and nothing tells sogen
when the guest lets go of any of them. So:

**A GUI arena block is never unmapped.** `macos_gui_arena` allocates blocks and never calls
`release_memory` on one. A released block is either:

- **recycled** — the emulator can prove every guest reference to it is dead, so the block may be handed
  to the next request; or
- **retired** — the guest may still hold a pointer into it, so the block stays mapped forever and is
  never handed out again.

The CF bridge's scratch is recycled, because `CFStringCreateWithBytes`/`CFNumberCreate` copy their input
— the `NoCopy` variants exist precisely because the plain ones do not keep a reference — so no CF
reference survives the call that read the scratch. A window's backing store is retired once
`window.context` is non-zero, recording that `SLWindowContextCreate` handed the guest a bitmap context
directly over it; Calculator's window is composited by sogen rather than drawn by the guest, so its
stores recycle in practice.

## Still open

- **`gui/macos_layer_contents.cpp` allocates rasters through `allocate_memory`/`release_memory` directly,
  not through the arena's recycle/retire discipline.** Its `abandon` path releases the pixel buffer while
  the `CGBitmapContext` built over those exact pixels may still be the guest's — the one remaining site
  that can hand a guest-drawn window background to something else.
- **A *hinted* allocation can still land inside an emulator-owned arena.** `mach_traps.cpp`'s
  `find_masked_base` retries with a non-zero hint, and a non-zero `start` turns off the
  `emulator_ranges_` skip in `find_free_allocation_base`; a guest `vm_allocate` carrying an alignment
  mask can therefore still be handed a range the emulator owns.
- **`mig_routines_vm.cpp`'s `vm_remap_routine` releases and re-maps a caller-named target range
  unconditionally.** The real `mach_vm_remap` only overwrites with `VM_FLAGS_OVERWRITE` set and
  otherwise answers `KERN_NO_SPACE`; the unconditional version was observed releasing live ranges in one
  faulting run.
- **A second, unidentified corruption channel remains.** After the fixes above, three faults captured in
  twelve runs show a per-thread malloc cache that reads back **all zeroes** — not pixel-filled — with no
  range the emulator ever claimed covering the cache address. The pixel channel documented above is
  closed; this second channel is not. Candidates, in the order they should be tested, are `vm_remap`
  above, a psynch prepost outliving its contender, and a message-queue pop-before-answer race in
  `mach_msg.cpp`.
- **The fault is load-dependent, not deterministic.** The guest clock is the host's own `CNTVCT_EL0`, so
  loading the host machine makes every guest timeout fire early; any reproduction attempt must control
  for concurrency, and a Calculator conclusion should be gated on repeated runs at matched load.
