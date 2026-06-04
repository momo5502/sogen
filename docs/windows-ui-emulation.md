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

Current cross-platform UI prototype proved backend seam works:
- Win32 backend
- SDL backend
- web backend

But visible controls still relied on hacks or host-native controls.
Real emulation goal is stronger: builtin controls, dialogs, and custom `WM_PAINT` should work from guest semantics, not backend-specific drawing.

## Big picture status

### Done

- generic `ui_backend` seam exists
- Win32 / SDL / web backends wired
- SDL is now the default backend when SDL3 is available
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
  - `NtGdiGetRealizationInfo`
  - `NtGdiSelectBrushLocal`
  - `NtGdiSelectPenLocal`
- `NtUserGetClientRect` added so `GetClientRect` returns correct dimensions
- DC-backed paint surface storage in `gdi_dc_state`
- SDL backend: guest paint surface is presented via `EndPaint` → `present_surface` → SDL texture
- **Root cause fixed**: `completion_NtUserMessageCall` was calling `present_window_surface` (host fallback) after WM_PAINT, overwriting the guest-painted texture. Now only `validate_window` is called.

### Current truth

- `custom-paint-sample` paint path executes fully:
  - `BeginPaint`
  - GDI brush/pen creation + selection via `NtGdiSelectBrushLocal` / `NtGdiSelectPenLocal`
  - `MoveToEx` via `NtGdiMoveToEx`
  - line syscalls (`NtGdiLineTo`)
  - fill syscalls (`NtGdiPatBlt`)
  - `EndPaint` → `present_surface` → SDL texture updated and rendered
- Host fallback no longer overwrites guest paint after WM_PAINT

## Non-goals for this checkpoint

Not trying to finish all of USER/GDI now.
Remaining work:
- text rendering (`NtGdiExtTextOutW` is placeholder)
- builtin control self-draw (Button, Static paint via real user32 code)
- region / clip path handling
- advanced ROP modes for PatBlt

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
