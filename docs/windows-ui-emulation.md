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

## Remaining blockers

- finish small-text / batched GDI text path so ordinary `TextOutA/W` works without forcing oversized `ExtTextOutW`
- decide where batch flush should happen generically, not only in sample-driven probing
- replace temporary debug-font text path with more correct font/text handling over time
- improve builtin control self-draw (`Button`, `Static`) via real user32 paint path
- add more correct clip/region handling
- add more correct ROP / brush / pen / DC semantics where builtin paint needs them

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
