# macOS window server

SkyLight's connection bring-up, the CALayer tree AppKit actually builds, sogen's software compositor,
and the signed-distance-field rasteriser that draws a SwiftUI Liquid Glass bezel. It sits above
[Mach IPC](macos-mach-ipc.md) and below nothing — this is the layer that turns a running AppKit process
into the pixels described in [`macos-emulation.md`](macos-emulation.md).

This document does not restate `src/macos-emulator/gui/`; it records the parts that are not recoverable
by reading it — the wire facts that forced the design, the rendering theories that measurement killed,
and the gaps the next stage inherits.

## Status

Reachable and measured: SkyLight's connection bring-up, a real CALayer tree with masks, a software
compositor with clipping and premultiplied blending, contents resolution for CGImages, CABackingStores
and ObjC template images, and an SDF rasteriser that draws Calculator's keypad in its measured colour.
Calculator composites a recognisable 230×408 window — see the subject table in
[`macos-emulation.md`](macos-emulation.md). Not modelled, by name, in §6 below.

## The connection

A GUI process reaches WindowServer over MIG, using SkyLight's `32000 NewConnectionPort` exchange.
sogen used to fake the whole bring-up
by intercepting `SLSMainConnectionID`/`SLSNewConnection` at the export level; it now lets SkyLight's own
code build the connection object and answers it at the MIG layer instead — the only way the
client-constructed event port could ever become known to sogen at all.

What sogen must remember from that exchange:

- **`32000`'s reply is strictly type-checked.** `msgh_id` must be `32100`, the complex bit must be set,
  descriptor count exactly 2, size exactly 64 bytes, remote port 0, and both descriptors'
  `(disposition, type)` must read `0x0011` (move-send, port descriptor). Anything else and the client
  gets `-300` and has no connection at all.
- **Descriptor 0 is a constructed port the client already built** and ships make-send in the request;
  descriptor 1 is a task identity token, copy-send.
- **`40202 RegisterClient`'s reply size is exact:** `0x50 + round_up(uuid_length, 4)`. An empty UUID —
  which is safe, because `CA::Render::Context::validate_server_uuid` is a no-op unless the process is an
  internal Apple build — means a reply of exactly 80 bytes.
- **Memory entries must be real and mappable.** `mach_vm_map` used to refuse any request carrying a
  memory-entry object; sogen now looks the entry up and hands back the range it was made over, because
  sogen has one address space and a shared page is shared by being the same page rather than being
  mapped twice.
- **`task_name_for_pid` and `task_create_identity_token` are load-bearing**, not optional extras — a
  `CAContext` cannot be built without them.

Removing the export interception also retired `_SLSGetEventPort` as a fake accessor: with a real
connection, the port it returns is the client's own, and that is the one input has to be delivered to.

## The layer tree

**AppKit never links a window's layer tree with `addSublayer:`.** Traced across three probe apps, the
split is 35 `addSublayer:` / 20 `insertSublayer:atIndex:` / 29 `setSublayers:` — and the root layer's own
children arrive **exclusively** through `-[CALayer setSublayers:]`.

The array `setSublayers:` is handed is `__NSArrayM`, a mutable deque, not the immutable `__NSArrayI` the
original decoder assumed. `__NSArrayM`'s count lives at a different offset than `__NSArrayI`'s; the old
decoder read a count from `+0x08`, which in `__NSArrayM` is a word that is **always zero**. Every
`setSublayers:` call therefore decoded as an empty array, and `replace_sublayers` dutifully detached
every child it was ever handed — silently, because "empty array" is a perfectly valid input. Reachable
layers went **1 → 54** (`appkitwin`) and **1 → 283** (Calculator) once the decoder learned to name a
class before reading its storage and dispatch on the layout that class actually uses. The decoder
carries seven measured layouts, validated against 157 arrays with zero refusals.

An array whose class is named but not in that table is **refused**, not emptied: the layer keeps the
sublayers it already had. That is the important half of the fix — the old failure mode destroyed a
subtree on an unrecognised input; the new one degrades to a stale tree and names the offending class in
the log, so the next Foundation layout change is a one-line diagnosis.

## The compositor

`macos_layer_compositor` walks the tree depth-first in sublayer order and, per layer: skips a hidden
layer, an opacity-zero layer or an empty-bounds layer; paints `backgroundColor` over the (corner-rounded)
bounds; paints `contents`; paints the border; pushes a clip when `masksToBounds`; recurses. Blending is
8-bit source-over with premultiplied alpha, and an over-bright source **saturates rather than wraps** —
`255 + div255(255 * 38)` used to compute `293` and truncate to `37`, painting a light-grey separator
bright yellow.

**Masks, implemented and measured.** Only the mask's alpha is read (an opaque black mask masks in
exactly as an opaque white one); the mask's own opacity multiplies and its sublayers contribute their
alpha; the mask shapes the masked layer's whole subtree, not just its own drawing; nested masks multiply
(0.5 over 0.5 measured alpha `0x40`); a mask is placed exactly where a sublayer would be, including the
masked layer's own `sublayerTransform`; and a mask is **not** a sublayer — assigning a current sublayer
as a mask removes it from `sublayers`, and the reverse also holds.

**`geometryFlipped` moves sublayers; it does not turn their contents over.** A flipped layer's transform
genuinely mirrors — `-[CALayer convertPoint:toLayer:]` agrees a child's local (0,0) maps to a different
root point under the flip — but contents sampling must stay upright regardless of the chain's handedness.
sogen's compositor had generalised the flip into `to_superlayer()`, so it entered the transform chain and
mirrored every descendant's raster, turning every glyph in both `appkitwin` and Calculator upside down.
The fix decides the raster's row order from the *handedness of the whole chain relative to the root*,
not from local y — which is a different rule from the one `renderInContext:` uses on the layer being
rendered *directly* (that entry point flips its own context; the compositing rule does not).

## Contents

A `CALayer.contents` object is one of three kinds, discriminated by `CFGetTypeID` — not by class name,
because `object_getClassName` answers `__NSCFType` for two of the three:

1. A **CGImage**, borrowed directly.
2. A **CABackingStore**, from which `CABackingStoreCopyCGImage` must be called and *its* result
   re-checked against the CGImage type id — a plausible width/height pair is not proof enough, because
   `CGImageGetWidth`/`GetHeight` are two-instruction accessors that read `+0x28`/`+0x30` unconditionally
   and validate nothing. Feeding it a wrong object once produced `CGImageGetHeight() == 6539417313` for a
   "1992-wide" image — two qwords of an AppKit `__DATA_CONST` page, nothing more.
3. Anything else is an ObjC instance, resolved through a guarded runtime accessor call: ask the
   object's class whether it implements `-CGImage` or `-image`, check the method's
   return-type encoding is a pointer *before* calling it, then validate the answer's type id.

**A `CATintedImage`'s image is an alpha-only mask, and its colour lives in `_tint`, not in the image.**
`CGImageIsMask` is true for it — 8 bits per pixel, no colour space. `CGContextDrawImage` paints a mask in
the context's **current fill colour**, and a fresh bitmap context's fill colour is opaque black — which
is exactly why every glyph resolved by the naive route rendered dark. The fix reads `-tint`'s components
host-side with the same offsets the tree already uses (`+0x38` count, `+0x48` components) and draws with
`CGContextSetRGBFillColor` instead of handing the borrowed `CGColorRef` back into CoreGraphics — handing
it back directly, inside the same guest-call chain, made every Calculator frame come out empty across
three deterministic runs.

## SDF layers

A SwiftUI button's bezel — and the whole of Calculator's keypad — is not a `backgroundColor` layer at
all. It is a merged signed-distance field: a `CASDFLayer` container holding several `CASDFElementLayer`
children, each contributing a rounded-box distance function that the container folds and paints with one
effect. The whole contract was measured against the real WindowServer:

- **Shape.** Each element's field is the signed distance to a rounded box built from its own `bounds` and
  `cornerRadius`, with the radius **unclamped** — proven by a degenerate case (`cornerRadius = 100` on an
  80×50 box) that produces a small rounded diamond, which only the unclamped formula predicts.
- **Coverage.** `coverage = clamp(0.5 - d_dev, 0, 1)`, a one-device-pixel-wide ramp centred on the
  surface, measured at four quarter-pixel edge offsets.
- **Combination.** Elements fold left-to-right in sublayer order with exactly two operations, `union`
  and `subtraction`; order matters — the same two elements applied in the opposite order turn a hole into
  no hole at all.
- **What SwiftUI actually uses is a tiny subset**: `mode = "bounds"`, `operation = "union"`,
  `mergeElements = false`, `gaussianRadius = 0`, `effectOffset = 0` — every element in every glass tree
  measured. The finished bezel is, to within 5/255 over a 4:1 backdrop range, a flat translucent fill
  of the merged shape; the rim highlight the full glass stack adds is a +2/255 bump one point wide.
- **Calculator's keypad capsules were in the tree the whole time.** Their effect is `CASDFGradientEffect`,
  which the rasteriser refuses to draw rather than guess at, so nothing was painted. Treating a refused
  gradient-effect field as a flat capsule and painting it the measured colour — `(52,51,49)` for the
  number keys, `(43,39,37)` for the chrome glass under a `CABackdropLayer` — brought all twenty keys in.

## What is not modelled

- **`CAPortalLayer`.** Its projection rule was measured against a `CGWindowListCreateImage` oracle in an
  isolated probe: a portal draws its source layer's whole subtree at 1:1 scale, source-bounds-centre to
  portal-bounds-centre, unclipped, with `hidesSourceLayer` removing the source from its own position.
  Implementing exactly that rule and A/B-testing it on Calculator — one binary, one environment variable,
  both arms run back to back — made the frame **worse** both times: two spurious white capsules in the
  titlebar went away, but five portals project and four sources are hidden, and the hidden sources carry
  Calculator's keypad, which came back displaced and truncated. `hidesSourceLayer` is the destructive
  half — honouring it is only correct if the projection lands where CoreAnimation puts it, and a frame
  that loses real content is worse than one with two spurious circles. sogen therefore records
  `sourceLayer`/`hidesSourceLayer`, counts every portal as "not modelled" in its composite diagnostic, and
  draws neither the projection nor the hiding.
- **`compositingFilter`.** 13 layers, 4 distinct filter objects, all shared-cache constants in a full
  Calculator run — but `renderInContext:` ignores it entirely (a `CIMultiplyBlendMode` over a green
  backdrop rendered as plain yellow), so there is no reference to implement it against.
- **`allowsGroupOpacity`.** 4 layers set it `YES`, and none of them has opacity below 1 with more than one
  child, so it has no observable effect to model in either measured subject.
- **`shadowOpacity`.** 445 calls across one run, every one 0.0.
- **Planar IOSurfaces.** sogen's IOSurface user client models the one plane layout every measured subject
  uses; a multi-plane surface reports itself and is not modelled.
- **`NSViewBackingLayerContents`.** Resolves to nothing by design — it is a container, and the pixels it
  stands for live on a child layer's own `contents` — but that child is itself unresolved in every
  measured Calculator run; see the operator-glyph fault below.
- **Giving a `CABackingStore` its own storage.** The store is empty because `CABackingStoreUpdate_` never
  reaches the code that would write pixels into it, and that call is upstream of anything sogen's
  interception can reach; asking the layer to draw instead was tried and measured to produce nothing for
  these layers either.
- **A `contents` object that is an `IOSurface`.** `CALayer` accepts one directly, and sogen already has a
  surface store (`mach/io_surface_user_client.hpp`) that could answer it without a guest call at all; no
  layer in any measured subject sets one, so there is nothing yet to measure the shape against.
- **Stroking a `CAShapeLayer`.** `strokeColor` and `lineWidth` are recorded; Calculator sets no
  `strokeColor` on any of its 42 paths, so there is nothing measured to implement against.

## Where the operator glyphs were

`÷ × − + = % +/-` are `CAShapeLayer`s whose `path` is one of CoreGraphics' two **element-list** forms,
filled opaque white. The digits next to them in the same grid are not: they are `CATintedImage` masks
blitted as rasters. That is the whole of the difference, and it is why one half of the keypad rendered
and the other half did not.

Both forms are now decoded and filled. Measured on 25G76 against `CGPathApply` over ten paths, four of
them the operator keys' own glyph outlines (`src/tools/macos-gui-probe/pathprobe.c`):

- **kind 8, inline.** Point count `u16` at +0x18, element count `u16` at +0x1a, the element types packed
  three bits each least-significant-first in a `u32` at +0x1c, the points as pairs of doubles from +0x20.
- **kind 9, on the heap.** Point count at +0x18, element count at +0x20, the points as pairs of doubles
  from the start of the buffer at +0x30, and the element types as bytes **descending** from the byte
  offset at +0x28: `type[i] == buffer[offset - 1 - i]`. This is the field two earlier probes did not
  find; it is at the far end of the same allocation the points live in, written backwards.

Both counts include one point per `close` -- CoreGraphics stores the subpath's start point again rather
than referring back to it. Neither form carries an affine: a creation transform is already folded into
the points.

Curves are flattened where the path is decoded, because the compositor cannot reach guest memory, and
every subpath is closed whether the element list said so or not -- which is what CoreGraphics fills. The
fill itself is a scanline sweep: each of four sub-scanlines per pixel row crosses every edge once, the
crossings are sorted, and the winding is carried along the row, so the sixteen samples of the same 4x4
supersampling the closed forms use cost one pass over the edges instead of sixteen. Both winding rules
are checked against `-[CAShapeLayer renderInContext:]` in `layer_compositor_test.cpp`.

## Four theories that were wrong

Chasing Calculator's missing button fills produced four hypotheses, each disproved by a specific
measurement, none of which was the actual cause:

1. **A wrong display-query answer.** Killed by three instructions of disassembly:
   `CACGContextEvaluator::suggested_format`'s `-1` sits behind a byte gate at `+0x48`, set only when the
   drawing callback actually painted something. Nothing about the display, the colour space or the
   render server is read before that gate.
2. **An opaque layer painted over the glyphs.** Killed by a full layer census: of 365 recorded layers, 52
   are unreachable orphans (every one a leaf with no parent and no children), 67 reachable layers are
   never painted for an explicit reason CoreAnimation would apply too (hidden, zero-size, opacity zero),
   and **none** of the layers needs sibling z-ordering (363 of the 364 layers checked sit at
   `zPosition` 0.0, and the one exception is an unattached orphan) — which `renderInContext:` ignores
   too, so there is no reference to check it against either way. Nothing is silently dropped.
3. **CoreUI failing to draw.** Killed by `_CUIDraw` being called **zero times on the host itself** — a
   stock `NSButton` on this SwiftUI-hosted release never draws from a `.car` catalogue at all, so a zero
   count is not evidence of a sogen fault.
4. **`CABackingStore` never being allocated.** Real — Calculator's `CABackingStore` objects genuinely have
   no storage, because `CACGContextEvaluator`'s recording context sees no paint operation to record — but
   not the cause of the missing key fills, because a button's background is a plain `backgroundColor`
   layer with no contents at all, never a backing store.

## The orange is real, and it is the window's activation state

An earlier round of this work recorded "there is no orange" as measured ground truth and used it to
justify a monochrome operator column. That conclusion was wrong, and the measurement that produced it
was of an **inactive** window.

Re-measured on 25G76 against the real Calculator, capturing the same window twice:

| | body | number keys | function keys | operator column | glyphs |
| --- | --- | --- | --- | --- | --- |
| active | `rgb(35,33,29)` | `rgb(72,71,68)` | `rgb(115,114,111)` | **`rgb(255,146,0)`, 7,835 px** | `rgb(255,255,255)` |
| inactive | `rgb(35,33,29)` | `rgb(50,48,45)` | `rgb(57,56,53)` | `rgb(57,56,53)`, no saturated pixel | `rgb(245,245,245)` |

The inactive row is the palette the earlier work wrote down as the app's appearance. Two capture traps
produced it and both are worth naming: `screencapture` of the whole screen shows the app's real state
only while it holds focus, and `CGWindowListCreateImage` on a window that has not redrawn since it lost
focus hands back the **stale inactive buffer** — two successive calls, one with the app active, returned
byte-identical inactive images until a keystroke forced a redraw. Both traps look like a measurement.

The orange is in the app's own client-drawn buffer, not a WindowServer effect: once the window redraws
while active, `CGWindowListCreateImage` carries the 7,835 orange pixels itself.

**The traffic lights are the same fact.** On the host their colour is a plain
`-[CALayer setBackgroundColor:]` on three 14x14 `CALayer`s — `(1.000,0.361,0.373)`,
`(0.204,0.780,0.349)`, `(0.980,0.784,0.000)` in an active dark window. In a Calculator run under sogen
the very same three 14x14 layers are handed `rgba(0.137,0.137,0.137,1)`, which is `rgb(35,35,35)`, and
that is exactly what sogen paints. Nothing about the traffic lights fails to draw; the guest asks for
the inactive grey.

So the earlier scan that found **zero saturated colours anywhere in Calculator's layer tree** was a true
reading of a guest that believes it is not the active application, not a fact about the release. Whatever
resolves `.tint(.orange)` and the widget colours does so client-side, from the app's activation state.

`-[NSApplication isActive]` is bit 2 of the `_appFlags` byte at ivar offset 284, and `-[NSApplication
init]` decides it at `init+1688` on 25G76: it sends `-_isActiveApp`, which tail-calls `_NXIsActiveApp`,
which is `GetFrontProcess()` followed by `SameProcess(front, kCurrentProcess)`. Only a yes reaches
`-setIsActive:YES`; nothing else in the launch writes the bit. Both halves are LaunchServices —
`GetFrontProcess` is `_LSCopyFrontApplication` and `SameProcess` is `_LSCompareASNsLong` — and with no
`lsd` the copy comes back null and `GetFrontProcess` falls through to `procNotFound`, so every GUI guest
came up in the background.

sogen answers both at the Process Manager layer now, beside the five HIServices routines it already
answered for the same reason (`src/macos-emulator/gui/macos_process_manager_routines.cpp`): the emulated
session holds exactly one process, so that process is the front process, and `kCurrentProcess` ({0,2})
resolves to it. Measured on the Calculator frame: the traffic lights come up `rgb(255,92,95)` and
`rgb(250,200,0)` — the zoom button is disabled in Calculator and stays grey in the real app too — and
four more key glyphs rasterise `tinted (1 1 1 1)` instead of untinted black. The whole frame goes from
zero saturated pixels to 312.

**The orange did not follow, and activation is not why.** The keypad is five `CASDFLayer` fields and
sogen paints every one of them with a constant in `macos_layer_compositor.cpp`, so no guest colour
reaches the capsules whatever the app's state is.

`CASDFGradientEffect._colors` (+0x10) is now readable, and it is not where the orange is. The array is
a lazily bridged Swift `[Any]`: an `__SwiftDeferredNSArray` whose `_heapBufferBridged` (+0x10) is still
nil and whose elements live in `_nativeStorage` (+0x18) as 32-byte `Any` existential boxes, the CGColor
reference in the first word. It would not name because **arm64e signs the isa whether or not the
nonpointer bit is set**, and `read_class_name` only masked the signature off when that bit was set — so
every object storing a raw isa, which is every Swift class instance, resolved its class to a signature
plus an address. Masking unconditionally fixes it; a raw pointer cannot leave the 36-bit user address
space, so the mask is a no-op on one. Read at present rather than at `-[CASDFLayer setEffect:]`, where
`_colors` really is still nil, all three keypad fields state the **same** three stops: opaque white,
opaque white, and white at alpha 0 at distances −1, 0 and 10. Nothing there distinguishes the operator
column, and because the stops are clipped to `d ≤ 0` a faithful render of that gradient would paint the
whole keypad opaque white. `MACOS_LAYER_MEASURED_CAPSULE_GLASS` is therefore standing in for the entire
unmodelled backdrop-and-blend pipeline, not for a colour that merely could not be read.

The orange exists in the guest and stays outside CoreAnimation. A heap scan of the live process finds
one `CGColor` of `(1.0, 0.575999975, 0.0, 1.0)` — sRGB `rgb(255,146,0)` — reached from a `CUINamedColor`,
which is Calculator's asset-catalog accent, plus eight copies of the matching `Color.Resolved`
linear-light `float4` `(1.0, 0.291249, 0.0, 1.0)`. A bounded six-hop walk over roughly 5,000 reachable
objects from each of the two keypad `SwiftUI.SDFLayer`s and each of the eight keypad `CASDFLayer`s
reaches none of them. On the host the same holds for five different tint spellings — `.glassEffect`
with `.tint`, `.buttonStyle(.glass)`, `.glassProminent`, `.borderedProminent` — every `CASDF*` effect in
the resulting tree holds only white, and `Mirror` shows the tint living in SwiftUI's Swift-side
`SDFStyle`, whose `blend` is a `_ColorMatrix`. The orange waits on reading that tree, not on the
CoreAnimation side.

Three faults remain, localised and named rather than guessed at: the function
keys render 8 levels too dark, because the top row shares one SDF field with the untinted number keys and
the tint distinction exists only in the shape of SwiftUI's Swift-side `SDFStyle` tree, which needs a Swift
reflection reader sogen does not have; the window body renders 13 levels too dark and neutral, because an
opaque `backgroundColor` layer paints over the chrome glass fill rather than under it — every portal in
this window is unmodelled, and a portal is exactly what relocates a layer's paint in the real compositor;
and eight titlebar and toolbar labels are missing from the rendered frame, because they are
`NSViewBackingLayerContents` objects whose pixels are their parent `NSViewBackingLayer`'s own
`-drawRect:` output, reachable only through `-[CALayer drawInContext:]` — a route measured to cost more
than it returns and, separately, to produce nothing for these particular layers anyway.
Calculator's operator glyphs
are not among them: `+/-`, `%` and `÷ × − + =` are `CAShapeLayer` element-list paths and they draw, the
frame reporting `8 shapes filled, 0 shape paths not modelled`.
