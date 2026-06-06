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

## UPDATE 2026-06-06 — builtin `MessageBox` renders and works end-to-end

`messagebox-sample` (`MessageBoxA(NULL, "Proceed?", "Question", MB_YESNO | MB_ICONQUESTION)`)
now renders the dialog with **Yes/No buttons, their labels, and the message text**, and
**clicking Yes returns `IDYES`** (the sample prints `clicked: yes`). The real user32
control wndprocs run and paint through the guest GDI path — no manual fallback drawing.

The sections below ("Current truth", "Builtin MessageBox Investigation Notes",
"Ntdll/User32 Callback Findings") predate this and are kept for history; several of their
"blockers" are resolved. The root causes that were actually blocking visible, interactive
controls turned out to be four independent issues (all verified with Ghidra on the **host**
`C:\Windows\System32\user32.dll` — note the emulator loads the *host* live system DLLs when
run with no `-e root`, not a captured `root` copy; the routing globals differ between builds
so always analyze the DLL the emulator actually maps):

1. **Controls never repaint after the dialog becomes visible.** user32's control wndprocs gate
   their paint body on the whole parent chain being visible
   (`ButtonWndProcW` → worker `FUN_18000a540` → drawability check `FUN_180008030`, which walks
   the window + every ancestor requiring `WS_VISIBLE`, style byte at WND `+0x1f` bit `0x10` =
   `0x10000000`; this maps to `USER_WINDOW dwStyle@0x1c`, `fnid@0x2a`, `spwndParent@0x30`).
   MessageBox creates its children during `WM_INITDIALOG` and they paint while the dialog is
   still hidden, so they correctly skip drawing. `NtUserShowWindow` then only invalidated the
   dialog itself. Fix: `invalidate_window_tree()` (in `user.cpp`) recursively re-invalidates the
   visible child subtree, called from `NtUserShowWindow` and `NtUserSetWindowPos(SWP_SHOWWINDOW)`
   after `WS_VISIBLE` is set.

2. **Button click hit-testing failed.** `is_button_window` (in `windows_emulator.cpp`) compared
   the literal `"Button"`, but `window::class_name` stores the raw ordinal-atom class (`#1`).
   Fix: also match `#1` so `find_button_child_at` works and clicking dismisses the dialog.

3. **Wrong return code + blank button labels — both from `SERVERINFO.MBStrings`.**
   user32 `SoftModalMessageBox` builds the dialog template by reading each button's
   `{caption, control id}` from `gpsi->MBStrings`: an array at `gpsi + 0x3A4` of
   `{ WCHAR achName[16]; UINT id @ +0x20; UINT uStr @ +0x24 }` (0x28-byte entries), fixed order
   OK, Cancel, Abort, Retry, Ignore, Yes, No, Close, Help, Try Again, Continue (ids 1..11; per-MB-type
   index tables `DAT_…b3c00/b3c5c/b3c64`). Our SERVERINFO left it zeroed → buttons got control id 0
   (so the dialog always returned `IDOK`) and empty captions (blank labels). The dialog proc
   (`MB_DlgProc` @ `0x1800530a0`) stores MSGBOXDATA in the dialog's `GWLP_USERDATA` on
   `WM_INITDIALOG` and calls `EndDialog(control_id)` on `WM_COMMAND`. Fix:
   `seed_messagebox_button_strings()` in `win32k_userconnect.cpp` populates `MBStrings`, called from
   `try_copy_client_pfn_arrays` **after** the client-pfn copy (the `apfnClientWorker` fill zeroes the
   overlapping tail). Confirmed at runtime that user32's `gpsi` == our `user_handles.get_server_info()`.
   The fragile caption→ID heuristic (`map_dialog_button_caption_to_id`) was removed.

4. **Missing GDI/USER syscalls surfaced once controls actually painted:**
   - `NtGdiGetDCDword` — gdi32 calls it for `GdiGetIsMemDc`; was unimplemented and halted. Returns 0
     (a real paint DC is not a memory DC).
   - `NtGdiPolyPatBlt` — button frame/press repaint. **Entry layout verified at runtime against this
     build's gdi32: `{ int x; int y; int cx; int cy; HBRUSH hbr@+0x10 }` (position + size), NOT a RECT
     with right/bottom** — button frame draws pass thin edges like `{x=96,y=4,cx=1,cy=20}` that only
     make sense as width/height.
   - `NtUserDrawIconEx` — stubbed to return success so the `SS_ICON` static finishes painting (icon
     pixels are not drawn yet).

### Still open after this update

- The message-box **icon** is not drawn (`NtUserDrawIconEx` stub).
- The dialog **background** may stay white instead of the gray dialog face.
- **System builtin-class atom resolution is not portable** (see next section).

## System builtin-class atom resolution (open, build-specific)

`normalize_builtin_window_class_name` hardcodes specific class-atom values (`#1`→Button,
`#2032`/`#2160`/`#12192`→Static, `#32770`→dialog). This breaks across Windows builds: the same
sample works with one system's user32 (static = `#2032`) but fails with another's (static =
`#12192`, "missing class"). Investigation findings:

- The builtin window classes are **never registered via a syscall** — `NtUserRegisterClassExWOW`
  appears zero times in the full startup+run trace. So the emulator never observes the class
  atom → name mapping.
- The class atoms (Button `0x1`, Static `0x7f0`=#2032 / `0x2fa0`=#12192, Dialog `0x8002`=#32770) are
  **user32-internal, build-specific integer atoms** (< `MAXINTATOM` 0xC000). user32 already holds
  them; it calls `NtUserGetAtomName(atom)` (emulator returns the `#NNNN` integer form) and then
  passes the atom to `NtUserCreateWindowEx`. Dialog templates store controls by **system class
  ordinal** (`0x80`=Button, `0x82`=Static via `FUN_18006c2f4` writing `0xFFFF,<ord>`); user32 maps
  the ordinal to its internal atom.
- The atoms are **not** present in our `gpsi`/`USER_SERVERINFO` (scanned the full `0x1B58` struct as
  both 16- and 32-bit; the only `0x7f0` hits were the low word of worker-pfn pointers like
  `0x1801207f0`). So user32 is not reading them from a table the emulator populates.
- `add_or_find_atom` assigns string atoms from `0xC000` up, so emulator-registered atoms never
  collide with these low integer atoms anyway.

Because there is no registration hook and the atoms live in user32-internal build-specific state,
the emulator cannot currently resolve them dynamically — the hardcoded list is the stopgap (each new
build's atom must be added). **Decisive next step for a real fix:** decompile the `NtUserGetAtomName`
caller / dialog class-ordinal resolution in user32 to find where the static atom (`0x7f0`) is
sourced — either `gpsi->atomSysClass` (possibly *beyond* our `0x1B58` SERVERINFO size, in which case
enlarge the struct and pre-register system classes with emulator-chosen atoms = fully portable) or a
user32-internal global baked at `DllMain` (then read it at startup or keep the hardcode).

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

Resolved by the 2026-06-06 update above: builtin `MessageBox` control rendering on the real
user32 paint path, button self-draw, click→`WM_COMMAND`→`EndDialog`, correct return code, and
button captions. Still open:

- portable system builtin-class atom resolution (see "System builtin-class atom resolution")
- message-box **icon** pixels (`NtUserDrawIconEx` is a success stub)
- dialog **background** fill (may render white instead of the gray dialog face)
- finish small-text / batched GDI text path so ordinary `TextOutA/W` works without forcing oversized `ExtTextOutW`
- replace temporary debug-font text path with more correct font/text handling over time
- add more correct clip/region handling
- add more correct ROP / brush / pen / DC semantics where builtin paint needs them
- `NtGdiGetDCDword` only returns the default (0); other indices (MapMode, GraphicsMode, …) would need
  real values if a control relies on them

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

1. Make system builtin-class atom resolution portable (see that section) so the hardcoded `#NNNN`
   list can be dropped.
2. Draw the message-box icon (`NtUserDrawIconEx`) and the dialog background.
3. Keep builtin control paint on the real callback/user32 path; do not reintroduce manual
   `Button` / `Static` drawing fallbacks.
4. Continue improving batched text and font handling.

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

Builtin message box sample (renders Yes/No + text; clicking Yes prints `clicked: yes`):

```cmd
build\release\artifacts\analyzer.exe -s build\release\artifacts\messagebox-sample.exe
```

MessageBox sample with root mapping:

```cmd
build\release\artifacts\analyzer.exe -s -e root -p c:\messagebox-sample.exe build\release\artifacts\messagebox-sample.exe c:\messagebox-sample.exe
```
