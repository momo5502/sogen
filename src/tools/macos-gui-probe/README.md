# macos-gui-probe

Reference programs and the measurement procedure behind `src/macos-emulator/gui/macos_gui_exports.cpp`.

Nothing here is built by CMake. `cgsdemo.m` is compiled by hand on a Mac and run natively; it is the
reference for both the export names sogen intercepts and the order a program calls them in.

## Regenerating the export table

The plan for this stage called for `ipsw dyld extract` plus `nm`. That is not needed: sogen reads export
tries out of the cache already, so the table is regenerated with the test binary itself and measures the
same bytes the emulator will.

```sh
cd build/release/artifacts

# Which images the cache holds, filtered by a regex over their install names.
PROBE_IMAGE=LIST PROBE_PATTERN='SkyLight|CoreGraphics' \
  ./macos-emulator-test --gtest_also_run_disabled_tests --gtest_filter='GuiExports.DISABLED_*'

# Every export of one image, with its trie flags.
PROBE_IMAGE=/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight \
PROBE_PATTERN='^_SLS' \
  ./macos-emulator-test --gtest_also_run_disabled_tests --gtest_filter='GuiExports.DISABLED_*'
```

`(no address)` marks an entry whose trie payload is not an image offset. Do not put one in the table:

```
_CGWindowContextCreateImage    0x0001872a8001 flags=0x8  (no address)
_CGWindowContextCreate         0x0001872a8001 flags=0x8  (no address)
```

Flags `0x8` is `EXPORT_SYMBOL_FLAGS_REEXPORT`. CoreGraphics re-exports both of these from SkyLight, so
the payload is a library ordinal; reading it as an offset gives an unaligned address, and the same one
for both symbols. The implementation to intercept is SkyLight's `_SLWindowContextCreate`.

`GuiExports.EveryEntryResolvesInTheHostCache` fails when a table entry stops resolving, which is how a
rename between macOS releases surfaces.

## appkitwin.m

A real AppKit window: `NSApplication` + one titled `NSWindow` + one `NSButton`, no storyboard. It is
the trace subject for the MIG id → export-name measurement (the real Calculator cannot be traced:
AMFI blocks direct exec, SIP blocks attach). It prints `APPKITWIN-ONSCREEN` after checking its own
window with `CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly)` and exits 1.5 s after
launch, so one run covers launch → first frame → idle.

```sh
clang -arch arm64 -O2 -fobjc-arc -o appkitwin appkitwin.m \
  -framework Foundation -framework AppKit -framework CoreGraphics
./appkitwin
```

## wstrace

A `DYLD_INSERT_LIBRARIES` interposer on `mach_msg`, tracing only the process it is loaded into.
`./wstrace.sh` builds both tools into `$WSTRACE_WORKDIR` (default `/tmp/wstrace`) and records one
appkitwin run twice:

- `wstrace.log` — every `mach_msg` send: `msgh_id`, the `send_size`/`rcv_limit` arguments, remote
  port, return code, thread, and two attribution frames (`caller` = the function containing the
  `mach_msg` call site, `outer` = one frame-pointer hop out, usually the export that carried the
  message). The request header is captured before the call; a send|receive overwrites the buffer
  with the reply, whose `msgh_id` is request+100. SkyLight's stubs leave the header `msgh_size` as
  `0xAAAAAAAA` stack pattern — use the `snd=` field, not `size=`.
- `wstrace-exports.log` — lldb batch run of `wstrace.lldb`: auto-continuing breakpoints over the
  `SLSTransaction*`/`SLPS*`/`SLS*`/CA export surface, giving the ordered export-call list.

Three interposer traps on this build, all handled in `wstrace.c`: `dlsym(RTLD_NEXT, "mach_msg")`
honors the interpose and returns the replacer (recursion), `mach_msg_trap()` from a non-platform
binary is EXC_GUARD-killed, so the original call is made as `mach_msg_overwrite(…, MACH_MSG_NULL, 0)`.

The id → export-name table produced from these logs, with routine names recovered from the MIG
subsystem descriptors in the cache images, lives at

## cgsdemo.m

An on-screen window from the SkyLight client API, no AppKit. Build and run natively:

```sh
clang -arch arm64 -o cgsdemo cgsdemo.m \
  -framework Foundation -framework CoreGraphics \
  -F/System/Library/PrivateFrameworks -framework SkyLight
./cgsdemo
```

Under sogen:

```sh
build/release/artifacts/analyzer --os=macos -s src/tools/macos-gui-probe/cgsdemo
```

## inputprobe.m

The subject of the input gate. An `NSApplication` subclass that prints from `-sendEvent:`, an
`NSButton` subclass that prints from `-mouseDown:`, a target/action that prints when it fires, and a
local `NSEvent` monitor — so a run says exactly how far an injected event travelled. With
`INPUTPROBE_RAW=1` in the environment it also spawns a thread polling `SLSGetNextEventRecord`, which
answers the different question of whether the datagrams reach SkyLight at all; that drains the queue
AppKit would otherwise read, so leave it off for the gate itself.

```sh
clang -arch arm64 -O2 -fobjc-arc -o /tmp/inputprobe inputprobe.m \
  -framework Foundation -framework AppKit \
  -F/System/Library/PrivateFrameworks -framework SkyLight

SOGEN_MACOS_ROOT=/tmp/sogen-macos-root-full SOGEN_INPUT_PROBE=/tmp/inputprobe \
  build/release/artifacts/macos-emulator-test --gtest_also_run_disabled_tests \
  --gtest_filter='EventStream.DISABLED_AnInjectedEventReachesTheGuestsOwnAppKit'
```

The measurements behind it are

## The rendering canaries

`paintprobe` and `cgsdemo` at 640x480 against a `(30, 30, 34)` background:

| probe | non-background | colours |
| --- | --- | --- |
| `paintprobe` | 174,800 | 11 |
| `cgsdemo` | 54,000 | 2 |

**The totals are not enough.** A channel permutation in the window bitmap keeps both numbers exactly
and still puts the wrong colour on screen, which is how `MACOS_CG_BITMAP_INFO_BGRA_PREMULTIPLIED` sat
wrong for as long as it did. Compare the palette as well: `paintprobe`'s eight bars are
`(hue, 0.35 + 0.5(1 - hue), 1 - hue)` for `hue = i/8`, so bar 0 is `rgb(0, 217, 255)` and bar 7
`rgb(223, 105, 32)`, and `cgsdemo` fills `rgb(255, 59, 48)`.

## Other probes

- `calcdemo.c` — a calculator laid out as real SkyLight windows, exercising the window path without
 CoreGraphics.
- `drawwin.c` — one SkyLight window with a drawing context, exercising the backing-store path.
- `iokitprobe.c` — drives the public IOKit client API one routine per phase, with a marker syscall
 between phases.
- `iokittrace.py` — lldb breakpoint callbacks dumping the raw IOKit MIG wire traffic `iokitprobe`
 sends and the replies that come back.
- `layerdump.m` — prints the live `CALayer` tree of a real AppKit window, property by property;
  produced the measured layer-compositor tables.
- `layergeom.m` — builds synthetic `CALayer` trees and asks CoreAnimation where a child's bounds
  corners land in the root's coordinate space.
- `sdfhost.swift` — dumps every `CASDF*`/`SDF*` layer that SwiftUI buttons and stock AppKit controls
 actually produce.
- `sdfprobe.m` — the class hierarchy, property list, ivar list and freshly-constructed defaults for
 QuartzCore's `CASDF*` classes.
- `sdfrender.m` — the rendering oracle: composites an on-screen grid of SDF cases and reads the frame
  back with `CGWindowListCreateImage`; the rendering oracle for the SDF contract.
- `sdfstyle.swift` — reads the Swift-side `sdfStyle`/`sdfEffects`/`sdfSubsets` state of
  `SwiftUI.SDFLayer`; produced the layer findings in
- `fsescape.c` — the filesystem containment oracle: as a guest, reads host-only canaries, writes and
  deletes through the root's symlinks, and tries `..` traversal, printing one `FSESCAPE` line per case.
  It measured the escape that `guest_file_system::escapes_root` closes, and it needs its canaries planted
  on the host first, since a case that finds nothing there proves nothing.

  ```sh
  clang -arch arm64 -O2 -o /tmp/fsescape fsescape.c
  echo canary | sudo tee /Library/Caches/sogen-canary-read.txt
  analyzer --os=macos -s -e /tmp/sogen-macos-root /tmp/fsescape
  ```
