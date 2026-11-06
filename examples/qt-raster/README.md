# Raster bridge example

This optional platform plugin lets a 64-bit guest send software-rendered widget
windows through `\\.\SogenQtBridge`. The emulator owns copied frames and a bounded
input queue. It requires no host GUI toolkit SDK. A headless probe checks frames and input; the optional
native presenter is described in `docs/qt-raster-native.md`. Browser presentation remains
a separate adapter.

The example uses the private platform interfaces from guest SDK version 6.8.3.
Build the plugin against exactly the SDK used by the calculator and runtime DLLs.
Other versions are not supported by this example. A 32-bit guest opening the device
receives `STATUS_NOT_SUPPORTED`.

## Build and run

Configure this directory separately from the emulator. For cross compilation,
provide the guest SDK prefix and a matching host tools installation:

```sh
cmake -S examples/qt-raster -B /tmp/raster-guest -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/examples/qt-raster/mingw.cmake" \
  -DCMAKE_PREFIX_PATH=/opt/guest-sdk/mingw_64 \
  -DQT_HOST_PATH=/opt/host-sdk/gcc_64
cmake --build /tmp/raster-guest
```

Deploy `calculator.exe`, its matching runtime DLLs, `qt.conf`, a `fonts/`
directory containing suitable TrueType fonts, and `platforms/qbridge.dll` to
`C:\qt-raster` in an existing emulation root. Runtime dependencies include the
core, GUI and widgets DLLs and the compiler runtime; resolve their transitive
imports from the same SDK. Do not add runtime binaries or roots to the repository.
Use this `qt.conf`:

```ini
[Paths]
Prefix=.
Plugins=.
```

Build the normal `qt-raster-probe` target. Then run each CPU backend explicitly:

```sh
qt-raster-probe /path/to/root kvm c:/qt-raster/calculator.exe /tmp/kvm.frame
qt-raster-probe /path/to/root unicorn c:/qt-raster/calculator.exe /tmp/unicorn.frame
```

The probe loads the guest plugin, waits for a guest-painted frame, queues `2+3=`
as key press/release pairs and requires successful guest exit and nonuniform
pixels. The calculator's `--probe` mode exits successfully only when its actual
display state is `5`. The optional output contains a frame packet followed by
premultiplied BGRA pixels. This proves painting and input through the device; it
does not prove visible presentation or interactive window behavior.

On a native guest OS, `calculator.exe --self-test` runs fourteen arithmetic cases
without loading the plugin. The ordinary calculator can use the stock platform
plugin for a visual reference.

## Wire and session contract

`qt_raster_protocol.hpp` defines a little-endian 64-byte header. Its fields are
six 32-bit values (magic, version, operation, window ID, request sequence and
payload size), eight signed 32-bit operation arguments, and a 64-bit session
identity. Version 2 assigns the identity to the final eight bytes. Peers must
match the version; there is no compatibility fallback.

Each open device has its own session. `Hello` uses sequence 1, identity 0 and
window ID 0; its reply assigns a nonzero identity and initial screen metrics.
Every subsequent request must carry that identity and a nonzero sequence greater
than the last accepted sequence. No wrap is allowed. `Hello` may reinitialize an
empty session, including after sequence exhaustion; previous identities then fail.
`End` requires all windows and queued input to have been removed. Device close
also releases the session. Active sessions reject emulator snapshots explicitly;
inactive handles restore with an empty session.

Create carries geometry, type, modality, transient parent and scale in arguments
0 through 7, with a UTF-8 title payload. Geometry, visibility, title, stacking,
focus requests and cursor shape update window metadata. A parent must exist and
cannot be destroyed before its children. Frames carry width, height, stride,
scale and dirty rectangle in arguments 0 through 7. Pixels are complete tightly
packed premultiplied BGRA images, at most 4096 by 4096. Stride must be width times
four; the dirty rectangle must fit the image. Scale is expressed in thousandths.

Accepted requests return a header acknowledgement, except `Hello` and `Poll`.
`Poll` returns one queued input event, or operation zero when empty, echoing the
request sequence and identity. Key events include bounded UTF-8 text. Unknown
operations, invalid lengths, stale identities and invalid geometry fail without
advancing the accepted sequence or changing window state. Too-small output and
failed guest writes preserve queued input for retry. The device checks page protection
over the complete input and the exact reply range before accessing guest buffers.

Limits are 64 live windows, 256 pending input events, 4032 input payload bytes per
event and 64 MiB of live window pixels per session. Requests are limited to a
64-byte header plus 64 MiB. A retained latest-frame reference may keep one deleted
window's frame alive; in-flight requests and previous frames temporarily add bounded
storage. The device reads pixels directly into an owned buffer and transfers that buffer
to the session. The guest plugin views the backing image while building its request,
avoiding an intermediate image copy. Destroying a window discards its pending events. Calls to session methods
must be serialized; device requests and probe callbacks use the emulator's kernel
lock. A future presenter must use the same synchronization contract.

The plugin supports raster widgets, timers, fonts and basic input dispatch. It
does not implement socket notifiers, GPU rendering, clipboard, input methods or
drag and drop. It advertises no GPU rendering capability. Window presentation and modal/popup input policy live in the optional native presenter,
separately from this plugin and the raster endpoint. Browser transport remains separate.
