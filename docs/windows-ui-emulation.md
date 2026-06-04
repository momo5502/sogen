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
- guest dialog flow works much better than before
- `MessageBox` no longer depends on export hook hack
- builtin control callback dispatch reaches real user32 code
- builtin `Button` / `Static` no longer crash during paint because correct extra bytes now allocated:
  - `Button` -> `cbWndExtra = 8`
  - `Static` -> `cbWndExtra = 8`
  - `#32770` -> `cbWndExtra = 30`
- temporary custom sample added:
  - `src/samples/custom-paint-sample/`
- minimal GDI syscall scaffolding added for custom paint investigation:
  - `NtGdiLineTo`
  - `NtGdiRectangle`
  - `NtGdiPatBlt`
  - `NtGdiExtTextOutW`
  - `NtGdiGetRealizationInfo`
- transient DC-backed paint surface storage added in `gdi_dc_state`
- Win32 backend now has temporary `present_surface()` path for top-level custom host window blit

### Current truth

- manual/button sample shows buttons visible on host, but still mixed with backend assistance
- builtin control paint path now executes much deeper inside real user32
- custom `WM_PAINT` sample reaches:
  - `BeginPaint`
  - GDI brush/pen creation
  - line/rectangle syscalls
  - `EndPaint`
  - emulator-side surface present
- surface data is being generated and presented through SDL/backend logs
- user-visible result for `custom-paint-sample` is still **empty window**

## Current branch state

Important files touched for current debugging/prototype work:
- `src/windows-emulator/process_context.hpp`
- `src/windows-emulator/syscalls.cpp`
- `src/windows-emulator/syscalls/gdi.cpp`
- `src/windows-emulator/syscalls/user.cpp`
- `src/windows-emulator/ui_backends/win32_ui_backend.cpp`
- `src/windows-emulator/ui_backends/sdl_ui_backend.cpp`
- `src/windows-emulator/windows_emulator.cpp`
- `src/windows-emulator/windows_emulator.hpp`
- `src/samples/custom-paint-sample/*`

## What we learned

### 1. Earlier builtin paint crash root cause

Builtin `Static` / `Button` paint path dereferenced `PWND->pExtraBytes`.
We had not allocated builtin class extra bytes.
That caused early failure / exception.

Fixing builtin class extra bytes let real user32 paint logic run deeper.

### 2. Current blocker shifted

After extra-bytes fix, blocker is no longer immediate builtin crash.
Now blocker is lower-level rendering/presentation correctness.

### 3. Simpler sample helps

`custom-paint-sample` isolates generic top-level `WM_PAINT` from builtin control complexity.
This is best ladder for current debugging.

## Current blockers

### Blocker A: custom paint sample still shows empty window

Observed facts:
- `WM_PAINT` dispatch happens
- `BeginPaint` / `EndPaint` happen
- temporary surface present path runs
- sample pixels change in logged buffer
- visible host window still appears empty

Likely remaining causes:
- top-level presentation path not actually drawing to visible backend window as intended
- wrong top-level backend selected / wrong host surface ownership path
- paint surface content exists but not mapped to displayed client area
- custom line path still not matching guest expectations well enough
- backend fallback layers may overwrite or hide presented surface

### Blocker B: line drawing semantics still rough

Current `NtGdiLineTo` implementation is temporary and incomplete.
It does not yet model real DC semantics well.
Need real handling for:
- `MoveToEx` / current point sync
- pen selection semantics
- clipping
- brush/ROP behavior where relevant

### Blocker C: text still not real

Current `NtGdiExtTextOutW` is only placeholder line drawing.
Real text rendering still missing.
That is intentional for now while chasing simpler geometry/present path.

## What needs to be done next

### Priority 1: make `custom-paint-sample` visibly draw

Use no-text sample first.
Need visible lines/rectangles before touching text.

Concrete next checks:
1. verify which backend is active during sample run
2. verify top-level window receives `present_surface()` and host/backend blits to same visible window
3. verify no later repaint clears visible content after present
4. verify line coordinates actually land inside visible client area
5. if needed, implement proper `NtGdiMoveTo` so current point is guest-correct

### Priority 2: finish minimal geometry path

Implement or verify:
- `NtGdiMoveTo`
- stable `LineTo`
- stable `Rectangle`
- `PatBlt` semantics needed by simple USER/GDI paths

### Priority 3: after geometry visible, tackle text

Then move to:
- `ExtTextOutW`
- `TextOutA/W`
- background mode / text color / font selection
- default GUI font semantics

### Priority 4: return to builtin controls

Once custom top-level paint is visibly correct:
- re-test builtin `STATIC`
- re-test builtin `BUTTON`
- inspect whether remaining issue is text only, or control frame logic too

## Temporary debugging aids currently in tree

Current branch contains temporary probes/logging for:
- UI paint dispatch traces
- builtin wndproc execution traces
- custom surface present traces
- backend present/blit traces
- GDI line traces

These should be removed after root cause understood and stable implementation lands.

## Non-goals for this checkpoint

Not trying to finish all of USER/GDI now.
This checkpoint is specifically about:
- proving real guest paint path
- reducing dependency on host-native control hacks
- finding exact missing pieces in generic custom paint and builtin control paint paths

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

## Short summary

Branch has moved from:
- fake/host-heavy control rendering

toward:
- real guest USER/GDI paint execution

But final visible pixel path still incomplete.
Immediate next win needed:
- make `custom-paint-sample` lines/rectangles visibly appear.
