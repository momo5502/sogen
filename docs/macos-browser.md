# macOS in the browser

Running the same macOS emulator described in [`macos-emulation.md`](macos-emulation.md) as a wasm64
module in a browser tab: the build constraints that make it possible at all, the synchronous range
bridge that pages in a 5.4 GiB shared cache from inside a guest memory fault, the idle-vs-deadlock
distinction that keeps a parked GUI guest reachable, and the service-worker cache that makes a second
visit free.

## Status

Reaches Calculator's idle point (its window composed, waiting on its run loop) in about 14 minutes,
against never arriving there at all before this work. What blocks a clickable Calculator today is a
pre-existing, intermittent `libsystem_malloc` fault that is guest-visible on native runs too — see
[`macos-emulation.md`](macos-emulation.md), "What does not work".

## The build

**`-DUNICORN_ARCH=aarch64` is required, and the failure mode without it reports nothing useful.** With
both `x86` and `aarch64` enabled, both softmmu targets end up in one wasm module and an aarch64
translation block can resolve the **x86_64** store helper instead of its own. That helper reads an x86
`CPUArchState` and TLB, produces a host address outside the module's linear memory, and the module traps
with `memory access out of bounds` four instructions into dyld. Nothing reports a problem *before* that:
the multi-architecture module links cleanly and runs static binaries fine, because a static binary's
stores hit an inlined fast path that never calls the helper at all. Reproduced both ways — `x86;aarch64`
traps, `aarch64` alone runs `/bin/echo` through real dyld to exit 0 — and enforced with a configure-time
check and message in `src/macos-web/CMakeLists.txt`.

**The target links `-g2`, not `-g0` or the build type's own `-g`.** DWARF costs nothing at run time:
the as-shipped `-g` build (45.2 MB) and `-g0` (4.3 MB, no debug info at all) run the embedded paint demo
in 18.00 s and 17.63 s respectively — a difference inside run-to-run noise.
Dropping DWARF is worth
**10× in download size** — 45.2 MB against 4.3 MB — for no measured runtime cost, because a browser has
to fetch what a native binary reads lazily off disk. `-g2` (4.9 MB) keeps function names — a wasm stack
trace and a CPU profile are unreadable without them — while dropping DWARF, which is the part a browser
has to transfer and a debugger, here, does not need. The `-g2 -O3` figure, 5.76 s, was measured only
after the range-bridge block cache below had also landed, so it does not isolate this link-flag
choice on its own.

## The range bridge

The guest's shared cache is 5.4 GiB; the browser never holds all of it. `dyld_cache_pager` demand-faults
16 KiB pages, and the fault handler reads them over a synchronous XHR into the worker.

**The bridge is synchronous because it has to be.** It is called from *inside a guest memory fault*, with
the guest suspended mid-instruction — there is no guest state to resume into if the read were asynchronous,
because the faulting instruction has not finished executing. This is also, mechanically, why the emulator
runs inside a **Web Worker** at all rather than the page's main thread: `FileReaderSync` — the only
synchronous file-reading primitive the platform offers — exists only inside a worker.

The direct consequence is that **IndexedDB and Cache Storage cannot be read from the bridge itself**, because
both are asynchronous APIs. Caching therefore has to sit one layer out, at the network layer (a service
worker intercepting `fetch`, below), or would otherwise require a synchronous reader such as
`FileReaderSync` over a `Blob` the cache already materialised.

## Idle is not deadlock

A guest that has finished launching and is parked on its own run loop waiting for a click is, from
inside the guest, in the *same state* as a guest that can never wake: a thread blocked on an empty Mach
port with no timeout and nothing else runnable. The mach-receive path used to halt unconditionally in
that state, which is correct for a headless analyzer run — nothing outside a command-line guest can ever
enqueue a message — and wrong for a GUI guest with a live front end behind it.

The discriminator is not about the guest at all: **can anything outside it still deliver?**
`ui_backend::can_deliver_input()` answers that per front end — false for the headless screenshot
backend, true once a browser page or an SDL window is actually attached
(`src/emulator-platform/platform/ui_backend.hpp`). When it is true, `macos_emulator::
park_for_host_input()` rewinds the thread's `pc` back onto its own `svc`, and polls the backend between
sleeps until an event actually reaches the guest. A headless run installs
nothing and halts exactly as before, with its full diagnostic.

**Headless analyzer runs must still terminate**, and do: `screenshot_ui_backend` only declares an input
source when the browser front end has actually attached one, so a run with nobody behind it ends at
idle rather than parking forever waiting for input that can never come.

This still needs a second call site. Calculator was measured coming to rest on a **dry workqueue pool**
in some runs and on the mach receive in others — which site a launched application stops at is not
fixed — and only the mach-receive site currently knows about host input. The workqueue-park site needs
the identical two-line change and has not yet landed, because it sits in a file another piece of work
owns.

## Throughput

Before this work the browser build never reached a composed window at all: it plateaued at ~23,500
syscalls and 0 presents and stopped making progress, decelerating to 0.12 syscalls/s. The cause was not
the interpreter, the frame cadence or per-syscall logging — it was that **`wait_until` refused to wait in
the browser and spun**, on the reasoning that blocking the shared worker thread would stall both frames
and input:

```cpp
#else
    // The browser build runs the emulator on the worker thread that also pumps frames and input, so
    // blocking it would stall both. An idle guest spins there instead...
    (void)deadline;
#endif
```

The premise is correct and the conclusion does not follow: `emscripten_sleep(ms)` does **not** block the
worker. It unwinds through ASYNCIFY, lets the worker's own message loop run for the duration, and
resumes — which is the *same primitive the frame pump already uses*, two functions away. Confirmed
independently while trying to profile the stall: Chrome's sampling profiler attached to the spinning
worker successfully, but `Profiler.stop()` never returned, because the worker never yielded long enough
to service the CDP message.

Computing the wait identically on both platforms and choosing only the sleep primitive
(`emscripten_sleep` in the browser, `std::this_thread::sleep_for` natively) took Calculator from a
permanent plateau to reaching a composed window in about 14 minutes — 127 threads and 5 presents, the
same shape as native's own idle point (85-98 threads, 5-6 presents) — roughly 7× native, against a
"before" that never converged at all.

A second, independent optimisation targeted the range bridge specifically. A CPU profile of the paint
demo — a self-contained probe that exercises the same dyld/shared-cache/compositor path as Calculator and
then exits — found **71% of one run's wall clock inside the synchronous XHR bridge**, against 5% in the
interpreter. Two changes, in order:

1. **Read ranges as bytes, not text.** A synchronous XHR's `responseType` is refused only in a *window*
   context; a worker is exempt. The old path fetched a 2 MiB page-in as `x-user-defined` text — a
   two-million-character string copied back one `charCodeAt` call at a time. **18.00 s → 14.58 s.**
2. **A bounded block cache for small reads, aligned to the pager's own chunk grid.** The cache
   header parser walks a 573 KB file a couple of bytes at a time, and each of those tiny reads was
   previously a full network round trip; answering reads smaller than a block out of whole cached blocks
   turns thousands of two-byte requests into one block fetch per region actually touched.
   **14.58 s → 5.70 s**, measured against the 1 MiB grid the cache launched with.

At `src/macos-web/emulator-worker.js:112-113`, the grid this cache actually holds today is
`RANGE_BLOCK_SIZE = 2 << 20` with `RANGE_BLOCK_BUDGET = 64 << 20` — 2 MiB blocks, 32 held at once. It was
not always 2 MiB: the code's own history is that a 1 MiB grid here against the service worker's 2 MiB
grid (below) cost real bytes — every small read pulled a whole 2 MiB service-worker block to answer a 1
MiB request from this cache, and a cold run fetched 1058 MiB instead of 692 MiB. Unifying both caches on
one 2 MiB grid, so a small-read block, a pager page-in and a service-worker cache entry are all the same
extent, is what closed that gap; the alignment is load-bearing, not incidental.

**3.16× overall**, and the run-to-run variance collapsed with it. This is a client-side cache
inside the worker's own memory — now grid-aligned with, but still logically separate from, the
persistent, cross-visit root cache below.

## The root cache

The range bridge above avoids re-reading the same bytes *within one run*; it does nothing for a second
visit, because nothing between the browser and the server retained anything. Chrome will not hold sparse
cache entries for a 1.7 GB resource in its ordinary HTTP cache even with correct validators, so the fix
sits one layer further out: `src/macos-web/sw.js`, a service worker with access to Cache Storage's much
larger quota.

**The crux is that `cache.put` refuses to store a 206** (partial content). So the service worker snaps
every read onto a fixed 2 MiB block grid — the same grid the guest's own pager reads in, so a page-in is
normally exactly one block — stores each whole block as a plain 200, and assembles whatever 206 the
caller actually asked for out of those blocks.

**Blocks are shared between URLs only where the server names the file behind them.** An emulation root
is built out of symlinks, so the same shared cache is reachable at more than one path — the guest opens
the cache directly, and a cryptex-based system also opens the identical bytes through a different mount
point. Keying blocks by URL cached the same content twice and warmed neither copy for the other, so
`serve.py` sends the file's device and inode as `X-Sogen-File-Id` and the worker keys on that when it is
offered. With no such header the key carries the URL, which shares nothing but is never wrong.

Three defects were found and are worth remembering precisely because they return the wrong bytes rather
than failing loudly:

- **A zero-length `Uint8Array` is truthy.** A guard written as `if (!body) return fetch(request)` lets a
  short assembly through as a "successful" 206 with an empty body and a self-consistent
  `Content-Range` — which a guest reads as a run of zeros instead of real data. The fix checks the
  assembled **length** against the length requested, not the value's truthiness, in both the assembly
  function and the caller.
- **A block key must include the file's size, not only a timestamp.** Every file in a macOS system root
  can share one mtime (a root built at one instant, or restored from one snapshot), so a validator built
  from mtime alone collapses every file in the root onto the same cache namespace — a read of one file
  can be answered with another file's cached block. Folding the size into the validator is what makes two
  different files with the same mtime resolve to different cache entries.
- **A validator is not a file identity, even with the size folded in.** Nothing in HTTP makes an `ETag`
  unique across URLs; it distinguishes versions of one resource. Since every file on a sealed system
  volume shares one mtime, size-and-mtime is the *size* — and on a macOS 26 root 2,325 of its 2,510
  framework `Info.plist`s then shared a key with a different framework's. Reading
  `CoreUI.framework`'s returned `DeveloperToolsSupport.framework`'s 1,338 bytes: the right length, so
  every short-read guard above passed, and CFBundle went looking for
  `CoreUI.framework/DeveloperToolsSupport`. `CFBundleGetBundleWithIdentifier("com.apple.coreui")` then
  missed, `_CFBundleEnsureAllBundlesUpToDate` rescanned all 812 loaded images, and `CUICreateRenderer`
  returned `NULL` — the only nil path into SwiftUI's `CatalogAppearance(named: "FauxVibrantDark")!`,
  which is where a browser Calculator run used to trap. Cross-URL sharing now needs the server to say
  which file is behind each URL.

Measured end to end on the paint demo, against a byte-metered server: with the cache off, a cold run
sends **692.4 MiB** over the network. Pressing `Prepare root` once (~52 s, 5.45 GiB into Cache Storage)
brings the very next run down to **0.3 MiB**, and every run after that to **0.0 MiB**. Against a server
on the same machine the cache is paradoxically slower in wall clock — Cache Storage per-read is slower
than localhost HTTP — but that trade fully inverts over a real network, where 692 MiB is roughly a
minute at 100 Mbit/s, *every run*, against effectively zero after the first. The cache is on by
default; `?rootcache=off` bypasses it without a rebuild.

## Input

The macOS backend shares its input abstraction with the Windows backend rather than inventing its own:
`ui_event` (`src/emulator-platform/platform/ui_backend.hpp`) is a Win32-shaped `{hwnd, message, wParam,
lParam}` tuple, and the macOS side is a **consumer** of that shape — it translates *out of* Win32
messages into the macOS event-record TLV the guest's own SkyLight decodes, not the other way around.

**`lParam` carries the point in the target window's client space, not the desktop's.**
`macos_translate_ui_event` reads that field as a Win32 client-space point and adds the window's own
origin back, the same way `sdl_ui_backend::map_window_point` already does for the native SDL front end.
A caller that packs the *desktop* point into `lParam` instead — sending an already-absolute coordinate —
lands every click at roughly twice its real offset from the window, because the origin gets added a
second time on top of a value that already included it. The browser's own input bridge
(`src/macos-web/app.js`) converts a desktop pointer event into the target window's client space before
packing it into `lParam`, for exactly this reason.
