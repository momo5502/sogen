# Windows UI emulation status

## Goal

Make guest USER/GDI own UI behavior and pixels.

Target end state:
- guest window procedures run normally
- guest USER/dialog/control logic decides behavior
- guest GDI drawing produces pixels
- backend only presents pixels and forwards input
- host-side control mirroring and fallback drawing can be removed

## Why this work exists

Cross-platform UI prototype proved backend seam works.
Historical path included:
- Win32 backend prototype
- SDL backend
- web backend

Current branch uses SDL3 for native host UI and web backend for playground/browser path.
Visible controls still relied on hacks or host-native controls.
Real emulation goal is stronger: builtin controls, dialogs, and custom `WM_PAINT` should work from guest semantics, not backend-specific drawing.

## Big picture status

### Done

- generic `ui_backend` seam exists
- SDL and web backends wired in current branch
- Win32 backend existed as prototype, but was removed in favor of SDL3 path
- SDL is now native default backend when SDL3 is available
- guest dialog flow works much better than before
- `MessageBox` no longer depends on export hook hack
- builtin control callback dispatch reaches real user32 code
- builtin `Button` / `Static` no longer crash during paint because correct extra bytes now allocated:
  - `Button` -> `cbWndExtra = 8`
  - `Static` -> `cbWndExtra = 8`
  - `#32770` -> `cbWndExtra = 30`
- temporary custom sample added:
  - `src/samples/custom-paint-sample/`
- GDI syscall scaffolding:
  - `NtGdiLineTo`
  - `NtGdiMoveToEx`
  - `NtGdiRectangle`
  - `NtGdiPatBlt`
  - `NtGdiExtTextOutW`
  - `NtGdiFlush`
  - `NtGdiGetRealizationInfo`
  - `NtGdiSelectBrushLocal`
  - `NtGdiSelectPenLocal`
- temporary debug bitmap font added:
  - `src/windows-emulator/debug_font.hpp`
- `NtUserGetClientRect` added so `GetClientRect` returns correct dimensions
- DC-backed paint surface storage in `gdi_dc_state`
- child-control paint is now composited into top-level window surface instead of relying on separate host child HWNDs
- SDL backend: guest paint surface is presented via `EndPaint` → `present_surface` → SDL texture
- **Root cause fixed**: `completion_NtUserMessageCall` was calling `present_window_surface` (host fallback) after `WM_PAINT`, overwriting guest-painted texture. Now only `validate_window` is called.

### Current truth

- `custom-paint-sample` now visibly renders geometry again:
  - border lines show
  - cross lines show
- `custom-paint-sample` paint path executes fully:
  - `BeginPaint`
  - GDI brush/pen creation + selection via `NtGdiSelectBrushLocal` / `NtGdiSelectPenLocal`
  - `MoveToEx` via `NtGdiMoveToEx`
  - line syscalls (`NtGdiLineTo`)
  - fill syscalls (`NtGdiPatBlt`)
  - `EndPaint` → `present_surface` → SDL texture updated and rendered
- child paint can feed top-level composed surface
- host fallback no longer overwrites guest paint after `WM_PAINT`
- direct text rendering path now exists via temporary 8x8 debug font in `NtGdiExtTextOutW`
- user validated forced direct text path visually: text appears and glyph mirroring bug was fixed
- current text gap is smaller-text GDI batching behavior, not raw top-level surface presentation
- `manual-messagebox-sample` runs through the native button/static paint path well enough for visible controls and text.
- builtin `MessageBox` is past the earlier missing `NtUserSetThreadState` syscall, but it is not working yet.
  - `NtUserSetThreadState(value, mask)` is now modeled as a per-thread USER state bitfield.
  - `NtUserGetThreadState(10)` returns that bitfield for the dialog cleanup path.
  - user32 creates the dialog and buttons, then uses predefined class atom `#2032` for the message-box icon/static control.
  - `#2032` now normalizes to the builtin `Static` class, matching the `SS_ICON` control path.
  - The next concrete blocker is `NtUserGetIconSize(icon, frame, &cx, &cy)`, reached while static/icon painting or setup runs.

## Builtin MessageBox Investigation Notes

IDA/user32 findings from the current checkpoint:

- `StartTaskModalDialog` / `EndTaskModalDialog` do not use `NtUserSetThreadState`; they only disable and re-enable top-level windows.
- The only `NtUserSetThreadState` callers found are in `DefDlgProcWorker`.
- `DefDlgProcWorker` uses the call as `NtUserSetThreadState(value, 0x4000)`, setting or clearing a dialog-related thread state bit. The return value is ignored at the inspected call sites.
- `DialogBox2` later calls `NtUserGetThreadState(10)` before an owner/foreground-window cleanup path.
- A blind no-op `NtUserSetThreadState` is not sufficient: it lets the sample proceed, but `MessageBox` returns `0` immediately.
- After adding the thread-state bitfield, verbose report output showed `MessageBox` got as far as dialog/control creation. It then failed on class atom `#2032`, destroyed the dialog, and printed `clicked: other (0)`.
- Mapping `#2032` to `Static` moves the failure forward to the next missing syscall: `NtUserGetIconSize`.

Expected next narrow implementation:

```cpp
BOOL NtUserGetIconSize(HICON icon, UINT frame, int* cx, int* cy);
```

For the current message-box icon path, returning a default icon size is probably enough to advance:

- write `cx = 32`
- write `cy = 64`
- return `TRUE`

The `cy = 64` value is intentional for the user32 static-icon path inspected in IDA: it halves the returned height before layout/drawing, so this models a 32x32 icon backed by a 32x64 icon/mask bitmap.

## Remaining blockers

- implement `NtUserGetIconSize` minimally and re-test builtin `messagebox-sample`
- expect possible follow-up icon/static paint gaps such as `DrawIconEx` behavior or icon-handle metadata
- finish small-text / batched GDI text path so ordinary `TextOutA/W` works without forcing oversized `ExtTextOutW`
- decide where batch flush should happen generically, not only in sample-driven probing
- replace temporary debug-font text path with more correct font/text handling over time
- improve builtin control self-draw (`Button`, `Static`) via real user32 paint path
- add more correct clip/region handling
- add more correct ROP / brush / pen / DC semantics where builtin paint needs them

## Backend parity notes

SDL and web still need separate host backends because their platform integration is genuinely different:

- SDL owns native windows, textures, and SDL event polling.
- web UI owns canvas compositing, worker `postMessage`, and browser keyboard/mouse events.

The backend contract should stay smaller than USER/GDI semantics:

- C++ USER/GDI code creates guest windows, paints pixels, composites child surfaces, and decides Win32 messages.
- backends present `ui_surface_desc` pixels for top-level windows.
- backends forward primitive input in a common shape.

Current parity work:

- web host now consumes the full `ui_window_desc` metadata instead of only rect/title/visible/enabled.
- web host converts `ui_surface_format::bgra8` into canvas RGBA before creating `ImageData`.
- web host forwards top-level-local mouse coordinates, matching SDL's coordinate shape.
- SDL no longer owns direct `Button` click-to-`WM_COMMAND` synthesis.
- direct child `Button` hit-testing for `WM_LBUTTONDOWN` now lives in `windows_emulator::handle_ui_event`, so SDL and web share that behavior.

The remaining design direction is to continue moving Win32 behavior out of host backends. Future paint or control fixes should generally land in the emulator-side USER/GDI path and then work in both SDL and web as long as the backend presents surfaces and forwards input.

## Next steps

1. Make batched text path work for normal `TextOutA/W` without sample hacks.
2. Re-test `manual-messagebox-sample` and inspect whether builtin control text now appears through guest paint path.
3. Re-test builtin `STATIC` / `BUTTON` paint after text-path progress.
4. Remove temporary paint/present probes and temporary sample hacks once path is stable.

## Non-goals for this checkpoint

Not trying to finish all of USER/GDI now.

## Recommended validation commands

Custom sample:

```cmd
build\release\artifacts\analyzer.exe -s build\release\artifacts\custom-paint-sample.exe
```

Manual message box sample:

```cmd
build\release\artifacts\analyzer.exe -s build\release\artifacts\manual-messagebox-sample.exe
```

MessageBox sample with root mapping:

```cmd
build\release\artifacts\analyzer.exe -s -e root -p c:\messagebox-sample.exe build\release\artifacts\messagebox-sample.exe c:\messagebox-sample.exe
```
