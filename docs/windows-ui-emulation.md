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
- The classic checkbox/radio **check glyph** is not drawn (`NtUserBitBltSysBmp` stub); the control
  box, label, and click/state handling work.

## System builtin-class atom resolution (resolved, portable)

**Fixed** in `resolve_builtin_class_atom` (`syscalls/user.cpp`). The earlier stopgap had
`normalize_builtin_window_class_name` hardcoding build-specific class-atom values
(`#2032`/`#2160`/`#12192`→Static); this broke across Windows builds because the same sample
resolved static = `#2032` on one system's user32 but `#12192` on another's ("missing class").

How user32 sources the atom (host `user32.dll`, dialog-creation worker `FUN_18002a070`): dialog
templates store controls by **system class ordinal** (`0x80`=Button, `0x81`=Edit, `0x82`=Static,
`0x83`=ListBox, `0x84`=ScrollBar, `0x85`=ComboBox, written as `0xFFFF,<ord>`). user32 turns the
ordinal into a class atom by indexing the WORD table `SERVERINFO.atomSysClass[ICLS]` at **`gpsi +
0x364`** with `(ordinal & 0x7F)`, then passes that atom to `NtUserCreateWindowEx`:

```c
class_atom = *(WORD *)(gpsi + (ordinal & 0x7F) * 2 + 0x364);
```

The atom values are assigned per build (Static = `0x7F0` here, `0x2FA0` elsewhere), which is why a
hardcoded list cannot be portable.

The fix reverse-maps an incoming integer class atom back to its canonical builtin class name by
reading that **same** `gpsi + 0x364` table and matching the atom against the ICLS-indexed entries
(0=Button, 1=Edit, 2=Static, 3=ListBox, 4=ScrollBar, 5=ComboBox). Because the lookup round-trips
through the exact memory user32 used for the forward mapping, it agrees with user32 regardless of the
build's atom values — no hardcoded atoms. Verified: the message-box dialog now creates `Button` and
`Static` controls with no "missing class" errors and no `#NNNN` aliases.

Confirmed empirically with a runtime probe at `NtUserCreateWindowEx`: incoming Button atom `0x1`
matched `atomSysClass[0]`, incoming Static atom `0x7F0` matched `atomSysClass[2]`.

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
user32 paint path, button self-draw, click→`WM_COMMAND`→`EndDialog`, correct return code,
button captions, portable system builtin-class atom resolution (see "System builtin-class
atom resolution"), and the window/dialog **background** fill (top-level surfaces are now filled
with the class background brush — gray dialog face instead of white). Still open:

- message-box **icon** pixels (`NtUserDrawIconEx` is a success stub)
- classic checkbox/radio **check glyph** pixels (`NtUserBitBltSysBmp` is a success stub; box, label,
  and click/state handling work)
- finish small-text / batched GDI text path so ordinary `TextOutA/W` works without forcing oversized `ExtTextOutW`
- replace temporary debug-font text path with more correct font/text handling over time
- add more correct clip/region handling
- add more correct ROP / brush / pen / DC semantics where builtin paint needs them
- `NtGdiGetDCDword` only returns the default (0); other indices (MapMode, GraphicsMode, …) would need
  real values if a control relies on them

## UPDATE 2026-06-07 — input routing consolidated into the emulated win32k layer

Pointer hit-testing is win32k's (kernel) job, not the host backend's and not the guest's
user32: in real Windows the raw-input thread hit-tests the window tree, sends `WM_NCHITTEST`,
and posts the mouse message **already targeted at the specific child HWND**. `DefWindowProc`
never forwards a click to children, so the emulator (which plays the kernel role) must do the
hit-test. The earlier web/SDL backends each re-derived this themselves, which was the actual
hack.

Now consolidated:

- `windows_emulator::route_pointer` is the single authority for mouse capture, child
  hit-testing, and window-local coordinate translation. Both backends only forward
  `(top-level window, top-level-local x/y, button-state)`.
- The duplicated `findChildTarget` / `resolveClientTarget` / `windowOrigin` tree-walks were
  deleted from both web hosts (`page/src/web-ui-host.ts` and `ui_backends/web_ui_host.js`).
- **`GetMessage`/`PeekMessage` `IsChild` filter fixed**: `peek_pending_message` now matches a
  message whose target is the filter HWND *or any descendant of it*, so `GetMessage(&msg, hWnd,
  …)` with a non-NULL top-level no longer silently drops child-control messages. Previously the
  filter was exact-equality only and the hwnd-filtered form lost all child input.

### Host-side USER logic still to move into the emulated USER/win32k path (TODO)

These are simplifications/cheats that currently live in host C++ and should grow into a more
principled USER/win32k model over time:

- **`route_pointer` is a simplified hit-test.** It does not send `WM_NCHITTEST` to the wndproc
  (so `HTTRANSPARENT`, draggable client areas, and custom hit-testing don't work), emits no
  `WM_SETCURSOR` / `WM_MOUSEACTIVATE`, and approximates z-order by "last matching child wins".
- **All mouse messages are now child-routed** through `route_pointer` (`is_pointer_message`
  covers `WM_MOUSEMOVE` / `WM_LBUTTON*` / `WM_RBUTTON*`, honoring `SetCapture`). It is still a
  simplified hit-test (see above bullet).
- **Dialog manager shortcut (now partly redundant).** `handle_dialog_message` + `complete_dialog`
  (`syscalls/user.cpp`) intercept `WM_COMMAND(BN_CLICKED)` / `WM_KEYDOWN(VK_RETURN/ESCAPE)` /
  `WM_CLOSE` in host code and write the dialog's `DLGINFO` end-flag + result directly. As of the
  2026-06-07 (2) cross-build fix, **button-click completion no longer relies on this**: the real
  `DefDlgProc` → `EndDialog` → modal loop now drives it (a button's `WM_COMMAND` is a synchronous
  `SendMessage` that never reaches `handle_dialog_message` anyway). The shortcut still backs
  keyboard (`VK_RETURN`/`VK_ESCAPE`) and `WM_CLOSE` dismissal, which go through the message queue.
  Follow-up: route those through the real `DefDlgProc` path too, then remove the shortcut.
- **`WM_COMMAND` control-id fixup** in `handle_ui_event` (`windows_emulator.cpp`) rewrites
  `wParam` with the child's control id from host code.
- **`invalidate_window_tree`** drives child-subtree repaint on `ShowWindow` / `SetWindowPos`,
  compensating for the lack of real win32k visibility/repaint propagation.

## UPDATE 2026-06-07 (2) — cross-build support: control id + dialog completion

Clicks worked with the host's live system DLLs (Windows 11, build 26x00) but failed with a
captured **Windows Server 2022 (20348)** emulation root. The earlier "web backend" click bug was
really this build difference (web used the Server 2022 root, SDL used the host DLLs); SDL with the
Server 2022 root reproduces it. Two build-specific gaps were fixed, both verified by decompiling
each build's `user32.dll`:

1. **Child control id field differs between builds.** A button's `ButtonWndProc` builds its
   `WM_COMMAND` from the child's control id, which it reads from a build-specific WND offset:
   Windows 11 reads `wID` at **`WND+0x140`**, Windows Server 2022 reads `spmenu` at **`WND+0x98`**
   (`(short)pwnd[0x13]` in the notify helper; `spmenu` is where real Windows stores a child's id).
   The emulator only wrote `wID@0x140`, so on Server 2022 every `WM_COMMAND` carried id `0` and the
   app's handler fell through to `DefWindowProc`. Fix (`NtUserCreateWindowEx` /
   `NtUserSetWindowLongPtr(GWLP_ID)`): child windows now mirror the id into **both** `spmenu` and
   `wID`, so the builtin wndprocs notify correctly regardless of the loaded build. This removed the
   need for the old `handle_ui_event` `find_button_child_at` `WM_COMMAND` synthesis — the **real**
   `ButtonWndProc` now drives `WM_COMMAND` on both builds.

2. **Builtin `MessageBox`/dialog never ended (real `EndDialog` path).** Once real button
   `WM_COMMAND` replaced the synthesis, the builtin message box stopped closing on click. Root
   cause: `NtUserSetDialogPointer` stored the DLGINFO pointer but never set the WND "has-DLGINFO"
   flag at **`WND+0x12` bit 0**. user32's ensure-dialog-info helper gates on that bit, so it
   re-allocated a fresh DLGINFO on *every* dialog message (observed: hundreds of
   `NtUserSetDialogPointer` calls). The modal loop and `EndDialog` then read/wrote different DLGINFO
   blocks, so `EndDialog` (which sets `DLGINFO+0x18 |= 1`, hides the dialog, posts `WM_NULL`) never
   reached the loop. Fix: `NtUserSetDialogPointer` now sets/clears `WND+0x12` bit 0 with the pointer,
   matching the kernel. The dialog now completes through the real `DefDlgProc` → `EndDialog` → modal
   loop path (no synthesis); `messagebox-sample` returns `IDYES` on click again.

Both fixes mirror real Windows semantics rather than working around it: a child's id genuinely
lives in `spmenu`, and the kernel genuinely sets the `WND+0x12` DLGINFO bit. Writing the id to both
`spmenu` and `wID` is a deliberate build-agnostic choice (the emulator can't know at create time
which `user32` build is mapped, and each build reads only its own offset). Verified end-to-end on
both the host (Win11) and Server 2022 roots with `manual-controls-sample` and `messagebox-sample`.

The temporary `[ui-event]` / `[capture]` / `[sogen-ui]` input-routing diagnostics used to chase
this down were removed in the same change; the branch carries no leftover debug logging.

## UPDATE 2026-06-07 (3) — capture routing across top-levels + correct WM_SETTEXT A/W marshalling

Two follow-ups from reviewing the cross-build work.

### Mouse capture is honored across top-levels

`route_pointer` translated a captured window's coordinates only via
`get_window_origin_relative_to_ancestor(captured, top_level)`, which returns `nullopt` when the
event was reported for a *different* top-level than the one owning the capture — and then fell back
to delivering the event to that other top-level. That breaks the `SetCapture` contract for
press/drag/release across emulator top-levels (a captured control could miss its button-up and stay
stuck). Fixed by translating through **screen coordinates** (origin relative to the root) so the
captured window always receives the event; for a captured window under the reporting top-level this
reduces to the previous offset.

### WM_SETTEXT is re-encoded to the target wndproc's encoding

`SetWindowTextA` on a builtin control (e.g. a `Static`) produced corrupted text. The investigation
ran through a couple of misleading layers before the real cause:

- **`LARGE_STRING.bAnsi` is not a fixed value.** user32's internal `_DefSetText(hwnd, text, bAnsi)`
  builds the `LARGE_STRING` for `NtUserDefSetText` from *the wndproc's own encoding*: it sets the
  `bAnsi` bit for an ANSI proc and clears it for a wide proc (the Unicode-only title path,
  `xxxSetWindowText`, additionally masks it with `& 0x7fffffff`). So `bAnsi` is authoritative **iff**
  the buffer reaching the proc already matches the proc's encoding.
- **The actual bug was on our side.** Tracing `SetWindowTextA` → `NtUserMessageCall(WM_SETTEXT)`
  showed `ansi=1` with an ANSI buffer (consistent), but the emulator dispatched it through the pinned
  `apfnClientA[21]` callback and forwarded the **raw ANSI `l_param`** straight to the wide
  `StaticWndProcW`. The wide proc then called `_DefSetText` with `bAnsi=0` over ANSI bytes →
  `read_def_set_text_string` saw `bAnsi=0` + ANSI text → corruption. The earlier byte-sniffing
  heuristic in `read_def_set_text_string` was only masking this.

Real win32k delivers a message's text to a wndproc in the proc's own encoding. The fix records each
window's wndproc encoding (`window.unicode_proc` — wide for builtin controls, the registered proc
otherwise) and, in `handle_NtUserMessageCall`, re-encodes the `WM_SETTEXT` string into a scratch
guest buffer when the caller's encoding (`ansi`) differs from the target proc's. The scratch buffer
is released in `completion_NtUserMessageCall`. With the payload now matching the proc, `bAnsi` is
consistent at `NtUserDefSetText`, so `read_def_set_text_string` trusts it and the byte-sniffing
special case is gone. Verified: `[defsettext]` for the status label flips to `bAnsi=0` + UTF‑16, and
control text renders correctly.

App windows whose class was registered with `RegisterClassExA` keep an ANSI proc, so an ANSI
`WM_SETTEXT` matches and is not converted — no behavior change for the common case. (Distinguishing a
`RegisterClassExW` app class would need the register-time A/W flag, which is not yet threaded
through; builtin controls, the affected case, are handled.)

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

1. Draw the message-box icon (`NtUserDrawIconEx`) and the dialog background.
2. Keep builtin control paint on the real callback/user32 path; do not reintroduce manual
   `Button` / `Static` drawing fallbacks.
3. Continue improving batched text and font handling.

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
