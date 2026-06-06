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
- builtin control callback dispatch reaches ntdll client thunks, but those thunks still need correct user32 client initialization before real builtin control paint can work
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
- `manual-messagebox-sample` exercises more of the USER/GDI path than before, but builtin control rendering still must not rely on manual fallback drawing.
- builtin `MessageBox` is past the earlier missing `NtUserSetThreadState`, `NtUserGetIconSize`, and `NtUserGetCursorFrameInfo` blockers, but it is not working correctly yet.
  - `NtUserSetThreadState(value, mask)` is now modeled as a per-thread USER state bitfield.
  - `NtUserGetThreadState(10)` returns that bitfield for the dialog cleanup path.
  - user32 creates the dialog and buttons, then uses predefined class atom `#2032` for the message-box icon/static control.
  - `#2032` now normalizes to the builtin `Static` class, matching the `SS_ICON` control path.
  - The current blocker is no longer a single missing syscall; it is correct user32 client callback initialization and builtin control paint.

## Builtin MessageBox Investigation Notes

IDA/user32 findings from the current checkpoint:

- `StartTaskModalDialog` / `EndTaskModalDialog` do not use `NtUserSetThreadState`; they only disable and re-enable top-level windows.
- The only `NtUserSetThreadState` callers found are in `DefDlgProcWorker`.
- `DefDlgProcWorker` uses the call as `NtUserSetThreadState(value, 0x4000)`, setting or clearing a dialog-related thread state bit. The return value is ignored at the inspected call sites.
- `DialogBox2` later calls `NtUserGetThreadState(10)` before an owner/foreground-window cleanup path.
- A blind no-op `NtUserSetThreadState` is not sufficient: it lets the sample proceed, but `MessageBox` returns `0` immediately.
- After adding the thread-state bitfield, verbose report output showed `MessageBox` got as far as dialog/control creation. It then failed on class atom `#2032`, destroyed the dialog, and printed `clicked: other (0)`.
- Mapping `#2032` to `Static` moved the failure forward to icon/static setup.

Implemented narrow syscall progress:

- `NtUserGetIconSize(icon, frame, &cx, &cy)`
  - returns `TRUE` for non-null icons
  - writes `cx = 32`
  - writes `cy = 64`
- `NtUserGetCursorFrameInfo(icon, frame, &rate, &frame_count)`
  - returns the icon handle for non-null icons
  - writes `rate = 0`
  - writes `frame_count = 1`
- `NtUserDestroyCursor(icon, flags)`
  - returns `TRUE` for non-null handles

The `cy = 64` value is intentional for the user32 static-icon path inspected in IDA: `xxxSetStaticImage` halves the returned height before layout/drawing, so this models a 32x32 icon backed by a 32x64 icon/mask bitmap.

Current runtime state:

- builtin `messagebox-sample` now creates the dialog window and child windows.
- The real guest paint path still does not make builtin button/static controls visible.
- A manual host-side/emulator-side paint experiment was added briefly:
  - intercepted builtin `Button` / `Static` `WM_PAINT`
  - called emulator GDI primitives directly
  - drew button rectangles and text from stored `window.name`
- That experiment made text/buttons visible, but it was a hack and has been removed.
- With that hack removed, visible builtin control rendering is still not solved.

## Ntdll/User32 Callback Findings

The important finding is that builtin class WndProc setup is not just a pointer lookup problem.

Observed current behavior:

- `SERVERINFO.apfnClientA/W` currently contains ntdll client thunks such as:
  - `NtdllButtonWndProc_A/W`
  - `NtdllStaticWndProc_A/W`
  - `NtdllDialogWndProc_A/W`
- In IDA, those ntdll thunks call entries in ntdll's `NtUserPfn` tables.
- Those table entries initially point at `UninitUser32Proc`, which prints `"User32 init not called"` and breaks.
- `user32!InitializeNtdllUserPfn` calls `ntdll!RtlInitializeNtUserPfn` with user32's A/W/worker callback arrays.

Experiments and conclusions:

- Directly replacing `SERVERINFO.apfnClient*` with user32's public WndProc arrays is not correct.
- That made class WndProcs point into user32, but `messagebox-sample` returned `clicked: other (0)` quickly.
- IDA explains why: `user32!ButtonWndProcW` immediately calls `ValidateHwnd`.
- `ValidateHwnd` depends on user32 client/global/shared state:
  - `TEB.Win32ClientInfo[8/9]`
  - `gSharedInfo`
  - `gpsi`
  - user32 handle-table metadata
- Our synthetic window objects are not enough for that public WndProc path.
- Patching ntdll's `NtUserPfn` tables from the emulator was also explored only as a debugging experiment and is not an acceptable solution.

Likely proper direction:

- Do not add manual control drawing fallbacks.
- Do not ad-hoc patch ntdll memory from emulator code.
- Make the user32 client initialization path happen in a Windows-like way, or emulate the relevant effects at the USER boundary with a principled model:
  - `UserClientDllInitialize`
  - `InitializeNtdllUserPfn`
  - `RtlInitializeNtUserPfn`
  - `RtlRetrieveNtUserPfn`
  - user32 shared info / handle validation state
- Keep callback dispatch through the ntdll/user32 callback convention instead of directly invoking public user32 WndProcs from host-side code.

## Remaining blockers

- builtin `MessageBox` control rendering still needs the real client callback/user32 initialization path
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

1. Model the user32 client initialization path cleanly enough that ntdll client thunks do not point at `UninitUser32Proc`.
2. Keep builtin control paint on the real callback/user32 path; do not reintroduce manual `Button` / `Static` drawing fallbacks.
3. Re-test builtin `STATIC` / `BUTTON` paint after client callback initialization is fixed.
4. Continue improving batched text and font handling once control paint reaches real GDI calls.

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
