# System cursors

`ui_backend::set_system_cursor(idc)` requests a standard cursor using the numeric
[IDC identifiers](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors).
The SDL backend supports arrow, text, wait, crosshair, diagonal/horizontal/vertical resize,
move, unavailable, hand and background-progress cursors. Other IDs select the arrow.

Guest cursor selection reaches this path through `NtUserSetCursor` for numeric stock
cursor resources from `USER32`. Selecting a stock cursor updates the host shape while
preserving the previous-handle return value and the existing visibility/display-count
behavior. Custom or named cursor resources and icons select the neutral arrow until
custom shapes are supported. Snapshot restoration and deserialization reapply the saved
shape and visibility using the same stock-resource classification.
[SetCursor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursor)
with a null handle still hides the pointer; selecting a shape while the display count is
negative leaves it hidden until `ShowCursor` makes the count non-negative.

Requests are applied during the next UI pump, including requests made before the first
pump or from a worker thread. The caller must pump, reset and destroy the SDL backend on
its UI thread. Cursor changes do not change visibility or mouse capture.

The backend owns its created cursors, reuses them until reset, and restores the borrowed
SDL default cursor before freeing them. Reset also invalidates queued cursor requests.
If the driver cannot create a requested cursor, the backend reports the failure and uses
the driver's default when available. Backends without cursor-shape support retain their
default behavior.

## Diagnostic

With SDL enabled, build `ui-cursor-diagnostic` in the configured binary directory. It
requires no guest executable, emulation root or Qt installation. Run it without arguments
and move across the four colored panels, or press 1–4, to select arrow, text, horizontal
resize and hand cursors. Escape closes the window.

`ui-cursor-diagnostic --check` verifies deferred requests, worker-thread requests, text
and resize selection, reuse, unknown-ID fallback, reset and destruction against SDL's
selected-cursor API. It returns 77 when video or system cursors are unavailable; CTest
reports that condition as a skip. An unsupported offscreen driver is not evidence that
visible cursor selection works. Run with a cursor-capable display for that check.

SDL's [cursor creation](https://wiki.libsdl.org/SDL3/SDL_CreateSystemCursor) and
[destruction](https://wiki.libsdl.org/SDL3/SDL_DestroyCursor) contracts require the main
thread. The request queue keeps those operations on the UI pump thread.
