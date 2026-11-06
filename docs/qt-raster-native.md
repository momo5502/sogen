# Native raster presentation

Build `qt-raster-native` with SDL enabled, then run the Windows calculator and matching
platform plugin described in `examples/qt-raster/README.md`:

```sh
qt-raster-native /path/to/root kvm c:/qt-raster/calculator.exe
qt-raster-native /path/to/root unicorn c:/qt-raster/calculator.exe
```

The launcher selects the guest `bridge` platform and uses the existing Windows executable,
plugin and runtime DLLs. It needs no host Qt SDK. Frames become ordinary SDL windows;
mouse, keyboard, focus, resize and close events return through the shared raster protocol.
Keyboard text is paired with its physical press so text input is not delivered twice.
UTF-16 surrogate pairs become one Unicode character; unpaired halves are discarded.
Both wheel axes and the Back/Forward mouse buttons are forwarded to the guest.

The presenter is an optional consumer of the raster session and scoped host-window routes.
It lives outside the emulator device implementation. The launcher delegates every UI method
to the normal SDL backend and synchronizes presentation between event pumps. It neither
changes normal analyzer presentation nor defines a dynamic device-provider ABI. The
integration depends on the raster endpoint and scoped routing contributions.

Call `native_presenter::update()` under `windows_emulator::synchronize()` between UI pumps.
Routed input already runs under that lock. The emulator and backend must outlive the
presenter. Destroy or clear the presenter before resetting the backend. Closing a device
or replacing its session removes its host windows on the next update. Host IDs are never
reused, and registrations are removed before host-window destruction. Frame storage is
owned by the session; the backend takes its own copy before returning from presentation.

Input waits in a bounded host backlog when the guest queue is full. Consecutive pointer
moves may coalesce; button and key transitions retain their order. Exhausting the backlog
reports an error instead of silently dropping a release. Focus loss releases held inputs.
Host resize requests use the existing `WM_SIZE` path and preserve logical client coordinates.

Popup menus and tooltips are composited into their owner frame, clipped to that host's
client area. Menus receive translated pointer and keyboard input; tooltips leave input
with the owner. Hiding an overlay restores the base pixels and owner focus while the
application remains active. Window-modal dialogs block their owner chain; application-modal
dialogs block other top levels in the same session. Child hosts are retired before owners.
Standard cursor shapes use the existing system-cursor interface, including hidden cursors.

Deploy the matching `window-behavior.exe` example beside the calculator to exercise menus,
a modal dialog, tooltips, text entry and the F6 shortcut through either backend. It emits
`WINDOW_STATE` and `WINDOW_LAYOUT` JSON records for observing guest state and control bounds.
The platform plugin retires transient children before an owner, preserving the device's
strict destruction order even when the toolkit destroys the owner first.

The wire layout remains version 2. Existing active-session snapshot rejection remains in
effect. Overlay pixels outside their owner, native window-manager transient/minimize policy,
custom cursor images, IME and general multi-window parity require further work. Browser
presentation is a separate adapter using the same Windows guest and wire protocol.
