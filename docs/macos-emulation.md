# macOS emulation

Entry point for sogen's macOS arm64 emulation programme: enough of Darwin's Mach-O loader, arm64 ABI,
BSD kernel, Mach IPC, WindowServer and AppKit surface to run a real, unmodified GUI application —
`/System/Applications/Calculator.app` is the reference subject — in `analyzer --os=macos` and in a
browser tab. Each layer below has its own document; this one is the map between them, the "what runs
today" summary, and the list of what still does not work.

Every number below is measured.

## What runs today

Measured on this repository's `analyzer --os=macos`:

| Subject | Arch | Reaches | Stops at |
| --- | --- | --- | --- |
| `appkitwin` | arm64 | exit 0 on its own, 1,378,534,860 instructions, a fully drawn window: 54 of 54 layers reachable, 74,028 non-background pixels / 119 colours — rounded body, titlebar, three traffic lights, the `OK` button with its grey fill, title upright and single | — |
| `Calculator.app` | arm64e | ~4.24e9 instructions, its real 230×408 window, 6 presents, 283 layers reachable, 225 drawn of 365 in the tree, 16 contents blitted — dark body, titlebar, traffic lights, the display showing `0`, `AC`, and `7 8 9 / 4 5 6 / 1 2 3 / 0 .` all legible. Stable 4/4 runs | idles in `CFRunLoop` waiting for input, 85 live threads — i.e. it has finished launching |
| `cgsdemo` | arm64 | exit 0, 54,000 non-background / 2 colours, `rgb(255,59,48)` | — |
| `paintprobe` | arm64 | exit 0, 174,800 non-background / 11 colours, palette `(0,217,255) (32,201,223) (64,185,191)` | — |

Input works end to end against a running probe: a synthesised click and keystroke reach the guest's own
AppKit — `-[NSApplication sendEvent:]`, an `NSEvent` monitor, `-[NSButton mouseDown:]` and the button's
target/action all fire.

## The architecture

**Mach-O loader** ([`macho-loader.md`](macho-loader.md)) maps the guest's main executable and
`/usr/lib/dyld` into an AArch64 guest address space and starts dyld's own entry point. Sogen applies no
fixups, no relocations and no symbol binding of its own — dyld does all of that, exactly as it does on
real hardware, which is the single biggest scope lever in the whole programme.

**arm64 backend** ([`arm64-backend.md`](arm64-backend.md)) is the Unicorn-hosted AArch64 guest CPU
underneath the loader. It is the only engine in the tree that can host an AArch64 guest at all — no
hypervisor backend has an AArch64 path — which also makes it the only option under wasm.

**BSD syscalls** ([`macos-kernel-core.md`](macos-kernel-core.md)) is the kernel surface above the
loader: `svc` dispatch keyed on `x16`'s sign, the two-page commpage, `sysctl`, the guest↔host filesystem
and fd table. Coverage is measured by a ratcheted test at **109 of 456** xnu-12377.121.6 syscalls;
sockets are refused by design, not missing — `sys_socket` accepts only `AF_UNIX`.

**Mach IPC** ([`macos-mach-ipc.md`](macos-mach-ipc.md)), alongside its two companion documents on
[concurrency](macos-concurrency.md) and [memory](macos-memory.md), is the port namespace, `mach_msg2`,
the MIG subsystems sogen answers, the IOKit/IOSurface user client and XPC bootstrap — the layer that
turns a Mach trap into a running multi-threaded process with daemons that answer or honestly refuse.

**Window server** ([`macos-window-server.md`](macos-window-server.md)) is SkyLight's connection
bring-up, the CALayer tree AppKit actually builds, sogen's software compositor, and the SDF rasteriser
that draws a SwiftUI Liquid Glass bezel. This is the layer that turns Calculator's layer tree into the
pixels in the table above.

**Browser** ([`macos-browser.md`](macos-browser.md)) is the wasm64 build of the same emulator running in
a Web Worker: the synchronous range bridge into the guest's 5.4 GiB shared cache, the idle-vs-deadlock
distinction that keeps a parked GUI guest alive for a click, and the service-worker root cache.

## How to run it

`src/tools/setup-macos-env.sh` does everything in this section: it checks the prerequisites, stages an
emulation root, configures and builds both trees, builds the playground, and prints the two commands
below filled in. Every step is idempotent, so re-running it after a `git pull` is the cheap way back to
a working tree.

```sh
src/tools/setup-macos-env.sh --serve
```

The rest of this section is what that script does, for anyone who needs a piece of it on its own or is
debugging why it failed.

### What the host needs

`brew install cmake ninja python node emscripten` covers the toolchain. Beyond that:

- **A macOS host.** The emulation root is built out of symlinks into a running macOS system, so staging
  one needs macOS. The emulator itself builds and runs on Linux and Windows too — it is only the root
  that has to come from a Mac.
- **Apple Silicon, for speed rather than correctness.** The guest is arm64. On an arm64 host it can run
  through Hypervisor.framework; elsewhere every instruction is interpreted, which works and is slow.
- **A shared cache at `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld`.** On a cryptex-based
  system — every macOS since Ventura — the cache is not under `/System/Library`, which is where dyld
  looks for it. `make-macos-root.sh` symlinks the real location into the staged root's expected path;
  it is also why pointing `--macos-root` straight at `/` does not work.
- **Chrome or Edge**, for the browser build. wasm64 is required and Safari and Firefox do not have it.

The build tree needs configuring once before the first build:

```sh
cmake --preset=release
cmake --build --preset=release

src/tools/make-macos-root.sh /tmp/sogen-macos-root-full

build/release/artifacts/analyzer --os=macos -v --gui \
  --root /tmp/sogen-macos-root-full --desktop-size 420x520 \
  --max-instructions 40000000000 --skip-syscalls \
  --screenshot /tmp/calc.png /System/Applications/Calculator.app
```

`--gui` installs the window-path interception; it used to be implied only by `--screenshot`, which made
a GUI run without a screenshot die somewhere unrelated. Calculator reaches the idle
window in the table above after a few minutes on typical developer hardware.

### Running it interactively

`--interactive` puts the guest's windows on the host instead of composing them for a file, and delivers
clicks and keys back into the guest. It implies `--gui`, and it uses the same SDL backend the Windows
and Linux front-ends always use:

```sh
build/release/artifacts/analyzer --os=macos -s --interactive \
  --root /tmp/sogen-macos-root-full --desktop-size 420x520 \
  --max-instructions 40000000000 /tmp/inputprobe
```

Such a run does not end on its own. An application idling on its run loop is only distinguishable from
a deadlock by whether input can still arrive, and with a window on screen it always can, so the guest
waits instead of halting; close the window or press Ctrl-C to end it.

`--interactive` and `--screenshot` are exclusive, and the analyzer says so rather than ignoring one: the
PNG is composed by the headless backend that the live window replaces. A window that is on screen is one
the host's own screenshot tool can capture.

`appkitwin`, `cgsdemo` and `paintprobe` are not built by CMake: they are small native macOS programs
under `src/tools/macos-gui-probe/`, compiled by hand with `clang` (see that directory's own README for
the exact invocations). Once one is built, run it the same way, in place of the Calculator path above —
each exits on its own, at the instruction count the table above records for it.

The browser build needs an Emscripten-toolchain build tree of its own, configured once through this
repository's own `emscripten64` preset — **not** by hand-deriving the toolchain path from `which emcc`,
which resolves to a Homebrew symlink one level short of the real toolchain file and fails configure with
"Could not find toolchain file". On a Homebrew emscripten install, point `EMSDK` at the shim
[`arm64-backend.md`](arm64-backend.md) ("Local emscripten setup") documents; an emsdk-managed install
needs no shim:

```sh
mkdir -p /tmp/emsdk-shim/upstream
ln -sfn /opt/homebrew/opt/emscripten/libexec /tmp/emsdk-shim/upstream/emscripten
EMSDK=/tmp/emsdk-shim cmake --preset=emscripten64 -DUNICORN_ARCH=aarch64 \
  -DCMAKE_CXX_FLAGS="-Wno-unused-command-line-argument"

ninja -C build/emscripten64 macos-web
(cd page && npm install && npm run build)
python3 src/macos-web/serve.py --port 8120 \
  --directory page/dist --macos-root /tmp/sogen-macos-root-full
```

The CMake build publishes the module into `page/public/`, and `npm run build` carries it into
`page/dist/` along with the service worker; serving `page/dist` is therefore what serves a current
module. Serving `build/emscripten64/artifacts` instead reaches the standalone page described below,
which is kept as a reference implementation and is not where new work lands.

Both commands run to completion on a fresh `build/emscripten64` as written above. The extra
`CMAKE_CXX_FLAGS` is a real, currently-necessary workaround, not caution: on Emscripten 4.0.23, `em++`
otherwise rejects `macos-emulator`'s precompiled header with `-Werror,-Wunused-command-line-argument` on
its own `-x c++-header` flag, unrelated to the toolchain-path problem above.

Open the served page in **Chrome or Edge**. It needs wasm64, which Safari and Firefox do not have, and
a browser without it fails at module instantiation rather than with a message the page can explain.

macOS is a third mode of the playground beside Windows and Linux: open **Settings** and select **macOS
Emulator**. The choice persists, so a reload comes back in the same mode. Then, in the macOS panel:

- **Prepare root** fills the service worker's block cache ahead of a run. It is optional — a run works
  without it — but it is what makes a second visit fast, and the panel warns when storage is not
  persistent, because the browser may evict the cache between visits.
- **Attach** takes either an `.app` bundle or a whole root, from the served root or from disk. A bundle's
  entry point comes from its `Info.plist` `CFBundleExecutable`; nothing hardcodes a binary name.
- **Run** starts the guest.

The paint demo reaches `exit 0` with a composed frame in about five seconds. Calculator takes several
minutes to its first window — the counters climbing (syscalls, threads, presents) are how you tell it is
alive — and then composes the same picture the native run above produces, pixel for pixel. Clicking a
digit updates its display.

See [`macos-browser.md`](macos-browser.md) for the range bridge and the root cache underneath all of
that. `src/macos-web`'s own page is still built and still works; it is kept as a control to A/B against
and is not where new work lands.

## The emulation root

Two symlinks — dyld plus the shared cache — are enough for a command-line binary, because everything it
links against lives inside the cache. They are **not** enough for anything with a window: AppKit's
`+[NSAppearance _initializeCoreUI]` reads `.car` asset catalogues out of `/System/Library/CoreServices`,
and CoreText reads fonts, and an app bundle reads its own resources, none of which the cache carries. An
app that cannot find them throws `NSInternalInconsistencyException` before its first window — which,
before this was diagnosed, surfaced as an unexplained `SIGSEGV` inside `__gxx_personality_v0`, the C++
unwinder running on an ObjC exception nobody caught.

`src/tools/make-macos-root.sh` builds the full root out of symlinks — nothing is copied, the guest only
reads — and is what every GUI subject above is run against.

## What does not work

- **`com.apple.lsd.mapdb`.** LaunchServices sends, gets sogen's per-message refusal, and sends again —
  hundreds of round trips with no window ever appearing. Failing the lookup instead parks libxpc forever
  in a synchronous send. Neither refusal shape works; it needs a real answer. This is the current wall
  between a headless AppKit probe and a window.
- **Calculator's over-release at instruction ~559M.** A freed object is released a second time during
  plist parsing; the signed-isa check only notices the corruption, it does not explain it. In progress.
- **`CAPortalLayer`.** Its projection rule is measured against a `CGWindowListCreateImage` oracle, but
  applying it to Calculator lost real content in two independent A/B runs, so it is recorded and not
  applied. Detail in [`macos-window-server.md`](macos-window-server.md).
- **Networking.** `sys_socket` refuses every domain but `AF_UNIX` by design — this is a scope decision,
  not a gap. A program that tries to open a TCP listener gets `EAFNOSUPPORT` back from `socket()`, the
  same way it would get refused inside a real sandbox, rather than sogen hanging or crashing.
- **A clickable Calculator in the browser.** The browser now reaches Calculator's idle point in about 14
  minutes (previously: never), but an intermittent, pre-existing `libsystem_malloc` fault currently wins
  the race to idle in every browser run measured so far. It is guest-visible on native runs too and
  belongs to neither the browser nor the idle logic.
- **Hardware-accelerated drawing.** The real WindowServer hard-links Metal; sogen presents
  software-rendered pixels only, and an app that renders through Metal will not work.
- **The `tidy` clang-tidy preset.** Has never run to completion; pre-existing failures in
  `src/backends/fex-emulator/` and `linux-emulator/syscalls/file.cpp` belong to other contributors'
  work.
