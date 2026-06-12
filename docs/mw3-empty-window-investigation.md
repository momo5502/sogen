# MW3 (open-iw5) empty-window investigation

Status of the effort to get **Call of Duty: Modern Warfare 3** (`open-iw5.exe -multiplayer`,
DXVK → GPU bridge → host Vulkan) to render a **first frame** in the emulator. The game window opens
but stays **empty**. This document records what we found, what we fixed, what we tried that didn't
work, the current blocker, and the assumptions behind the analysis so the next person doesn't have to
re-derive it.

Related: [`gpu-paravirtualization.md`](gpu-paravirtualization.md) (the bridge architecture).

## TL;DR

1. **Two real emulator/bridge bugs were found and fixed** along the way:
   - A **lost-wakeup in `NtAlertThreadByThreadId`** that deadlocked any contended critical section
     (committed, `46441c66`).
   - The **GPU bridge never signalled the acquire semaphore/fence** in `vkAcquireNextImageKHR`, so
     DXVK's render submit (and the frame fence it signals) never completed → render deadlock under
     WHP (fix in the working tree, uncommitted at time of writing).
2. **Critical gotcha:** the default backend is the **`unicorn` interpreter**, which is ~20× slower
   than the **WHP** hypervisor backend. Most of the apparent "stuck/slow" behaviour during the
   investigation was just interpreter slowness. **The render deadlock only reproduces under
   `EMULATOR_WHP=1`.** Always reproduce rendering issues under WHP.
3. **Current state:** after both fixes, under WHP the prior total freeze is gone — the game now runs
   and **exits on its own (clean `exit 0`)** without ever presenting a frame, while logging a burst
   of `!!! BAD PORT` (an unrecognised ALPC port). Why it exits, and whether the `BAD PORT` probe is
   the trigger, is **not yet understood**. No first frame has been presented.

## Symptom

`open-iw5.exe -multiplayer` opens an SDL window that stays empty. The renderer (DXVK) draws only its
init frame; the actual menu/UI never appears.

## Reproduce

```
# Real backend (hypervisor) — REQUIRED to repro the render deadlock:
cd build/release/artifacts
EMULATOR_WHP=1 analyzer.exe -c -ss "C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Modern Warfare 3\open-iw5.exe" -multiplayer
```

- Backend is chosen in `src/backend-selection/backend_selection.cpp`: default `unicorn`;
  `EMULATOR_WHP=1` → WHP; `EMULATOR_ICICLE=1` → icicle. **Without `EMULATOR_WHP=1` you are testing
  the slow interpreter and will draw the wrong conclusions about "stalls".**
- The guest `vulkan-1.dll` is a symlink to `build/release32/artifacts/vulkan-shim.dll` (the bridge
  shim). If that symlink is broken/renamed, the game takes a different display path and fails
  differently — verify it first.
- `open-iw5.exe` is built from `C:\Users\mauri\Desktop\open-iw5`; `generate.bat
  "--copy-to=<MW3 dir>"` makes the build copy the exe into the Steam dir the analyzer runs.

## Diagnostic tooling built for this (some left in tree, env-gated)

- **`EMULATOR_LOG_STACKS`** (`windows_emulator.cpp`, uncommitted): before each thread switch, walks
  every guest thread's **32-bit** stack by reading `teb32->NtTib.StackBase/StackLimit` and scanning
  the committed stack for values that (a) resolve to a loaded module via `mod_manager` and (b) are
  **CALL-preceded** (`E8` at `v-5`, or `FF /2|/3` at `v-2..v-6`). Heuristic (no ESP, so it mixes live
  and stale frames), but it reliably exposes *where threads are parked* and whether they move between
  dumps. **This was the workhorse.**
- **`EMULATOR_LOG_READ`** (`file.cpp`, uncommitted): logs every `NtReadFile` as
  `READ t=<ms> h=<handle> off=<offset> len -> bytes`. Used to prove the fastfile loader advances to
  EOF (not an infinite re-read) and to time the load phases.
- **GPU-bridge first-frame milestone** (`gpu_bridge.cpp`, uncommitted): `handle_queue_present` does
  `force_print("GPU bridge: first frame presented")` on the first successful present and every 100th
  after. `force_print` bypasses `--silent`/concise, so it shows even in `-s` runs. **This never
  fired** — we have never reached a present.
- **In-guest watchdog** (`open-iw5/src/module/watchdog.cpp`): attempt to capture precise callstacks
  from inside the game. Two approaches tried:
  - Thread enumeration via `CreateToolhelp32Snapshot` + `OpenThread`/`GetThreadContext` — **does not
    work in the emulator** (cross-thread enumeration unsupported; produced zero frames).
  - Hooking `NtWaitForAlertByThreadId`/`NtWaitForSingleObject` via minhook to capture the current
    thread's stack — **minhook could not patch the WoW64 ntdll syscall stubs**, so the hooks never
    installed. The emulator-side `EMULATOR_LOG_STACKS` scan was used instead.

## What we found, by layer

### 1. Critical-section deadlock — `NtAlertThreadByThreadId` lost wakeup (FIXED, `46441c66`)

Under the interpreter, the main thread (tid 1) and an iw5 worker (tid 11) were both parked forever in
`RtlpWaitOnCriticalSection` (ntdll `+0x6ec23 → +0x6ed24 → +0xd14b8 → +0xd1f05`), in the UI/asset-init
path (`FUN_00592630` → the `FUN_00564ae0` hash-table op under critical section `#0xe`).

Modern ntdll critical sections deliver their wakeup via `NtAlertThreadByThreadId` /
`NtWaitForAlertByThreadId`. The emulator only recorded the alert **if the target was already parked**
in the wait (`if (t.waiting_for_alert) t.alerted = true;`). ntdll routinely alerts a thread that is
*about to* wait (the release-then-wait race), so an alert that arrived before the wait was **dropped**
→ the waiter blocked forever → every contended lock could deadlock.

**Fix:** record the alert unconditionally (sticky) and have `NtWaitForAlertByThreadId` consume a
pending alert without blocking. This matches the kernel's race-free semantics. Confirmed: the
critical-section deadlock no longer occurs.

### 2. The "90-second stalls" were the interpreter, not a hang

After fix #1 we measured (via `EMULATOR_LOG_READ`) a load that reads the first fastfile to EOF in
~10 s, then a **~90 s window with no I/O**, then a burst of ~23 000 reads. This looked like a hang.
It was not: it was the **`unicorn` interpreter** grinding through a CPU/heap-heavy fastfile-decompress
phase (`sub_71F940`, a codec driven by a runtime callback table, looping `malloc`). Under **WHP** the
CPU phases collapse and the game blows past loading into DXVK render init. **Lesson: always test
rendering under WHP.**

### 3. Render deadlock — acquire semaphore/fence never signalled (fix in working tree)

Under WHP, after loading, **every guest thread froze** for 130 s (23 evenly-spaced stack dumps; the
scheduler kept running, so it is a *guest-level* deadlock, not a host-call hang). The decisive frame:
**tid 14 (`dxvk-submit`)** parked in our shim's `vkQueueSubmit2`/wait path → GPU-bridge IOCTL, while
tid 1 (main) waited on an event for the menu-zone load, and the DXVK render/shader threads (tid 13,
18–21) sat in `d3d9.dll`.

Root cause: `vulkan_host::acquire_next_image` handed out an image index round-robin but **never
signalled the semaphore/fence** DXVK passed to `vkAcquireNextImageKHR` (the shim explicitly ignored
them). DXVK makes its render submit GPU-wait on that acquire semaphore; with it never signalled, the
render command buffer never executes → the **frame fence never signals** → DXVK's `vkWaitForFences`
blocks forever → the whole frame pipeline deadlocks, and since there is no real present engine,
nothing ever signals it.

**Fix (working tree):** thread the acquire semaphore + fence object ids through the protocol
(`acquire_next_image_request`, `protocol_version` 15 → 16), the shim, and the bridge; in the host,
signal them immediately with an empty `vkQueueSubmit` (the images are always available, so signalling
on acquire is correct). After this, the WHP total-freeze is gone.

## Current blocker (unresolved)

With both fixes, under WHP the game **no longer deadlocks** — it runs and then **exits cleanly
(`exit 0`)** without ever presenting a frame. The only notable log output (silent mode) is:

- 118× `vulkan-shim: no implementation for Vulkan function: …` — DXVK probing optional entrypoints
  (NV latency, fullscreen-exclusive, keyed-mutex, `vkWaitForPresentKHR`, …). Returning null is
  expected for optional functions, but **needs auditing** for any DXVK actually depends on.
- 12× `!!! BAD PORT` — an **unrecognised ALPC port** (`port.cpp` `dummy_port` →
  `STATUS_NOT_SUPPORTED`). The run *ends* with a burst of these. The port **name is not logged**, so
  we don't yet know which service it is or whether the failed probe is what makes the game quit.

Open question: **why does the game exit instead of staying on the menu?** Candidates: a failed
service probe (`BAD PORT`), a DXVK feature it needs but the shim returns null for, or a `Com_Error`
that shuts down (note: `open-iw5`'s `patches.cpp` hooks the Com-error path to `_Exit(1)` — but we saw
`exit 0`, so this is more likely a normal `ExitProcess`, i.e. the game *chose* to quit).

## Which thread is the issue

- **Before fix #1:** tid 1 (main) + tid 11 (iw5 worker) — deadlocked on critical section `#0xe`.
- **Before fix #3 (under WHP):** **tid 14 `dxvk-submit`** is the keystone — blocked waiting on a
  frame fence that never signals because the acquire semaphore was never signalled. Everything else
  (main waiting for the menu load, the DXVK render/shader threads) is downstream of it.
- **Now:** no single stuck thread — the game exits. The next investigation is about the **exit cause**
  and the **`BAD PORT` / missing-Vulkan-entrypoint** angles, not a hung thread.

## What we tried that did **not** work / dead ends

- **In-guest thread enumeration** (`CreateToolhelp32Snapshot` + `GetThreadContext`): unsupported in
  the emulator — produced no frames.
- **In-guest hooking of ntdll wait syscalls** (minhook on `NtWaitForAlertByThreadId` /
  `NtWaitForSingleObject`): the hooks never installed (WoW64 syscall stubs resist a 5-byte patch).
- **Heuristic full-committed-stack scan**: works for *locating* parked threads but cannot cleanly
  separate live from stale frames (no ESP). Good enough to find the deadlocks; not a true unwind.
- **Long silent runs** to "wait it out": misleading on the interpreter (looks like slow progress);
  under WHP the freeze is a true deadlock, so waiting never helps.

## Assumptions

- The single emulator vCPU serialises all host Vulkan calls, so host-side data races are not a
  concern, but **one blocking host call would freeze every guest thread** (the bridge deliberately
  keeps submit/present non-blocking; keep it that way).
- The swapchain is **faked**: offscreen images, and "present" is a GPU copy-to-buffer + readback that
  the UI backend blits to the SDL window (`vulkan_host::queue_present` → `ui().present_surface`).
  Therefore any GPU-side sync object DXVK expects the *presentation engine* to signal (acquire
  semaphore, and the present's wait semaphore) must be signalled **by the bridge**, or DXVK hangs.
- iw5mp is loaded at base `0x400000`; the IDA database
  (`open-iw5/build/bin/Win32/Release/iw5mp.exe.i64`) matches running addresses directly.

## Likely-next issues (educated guesses, unverified)

- **Present wait semaphore is also ignored.** The shim's `vkQueuePresentKHR` drops
  `pPresentInfo->pWaitSemaphores`, and the host readback submit does not wait on it. The render-finished
  semaphore therefore gets signalled but never waited → on a real driver it accumulates / triggers
  validation errors and may misbehave once frames actually flow. Mirror the acquire fix: have present
  consume (wait on) its wait semaphores.
- **The `BAD PORT` ALPC probe** may need a `noop`/stub port entry in `port.cpp` (log the port *name*
  first to identify it).
- **Audit the 118 "no implementation" entrypoints** for any DXVK hard-depends on for its present path.

## Recommended next steps

1. Log the **ALPC port name** in `port.cpp`'s `dummy_port` and identify the `BAD PORT` service; decide
   whether to stub it.
2. Find the **exit cause** under WHP with `-c` logging (the run is fast): capture the last emulator
   events before `ExitProcess`, and check the game's own `…_mp.log` in the MW3 dir.
3. Mirror the acquire fix on **present**: have `vkQueuePresentKHR` / the host present path **wait on**
   the present wait semaphores.
4. Once a frame presents, the GPU-bridge first-frame milestone will fire (`GPU bridge: first frame
   presented`).

## Working-tree state at time of writing

- Committed/pushed on `gpu-bridge-dxvk-entrypoints`: `46441c66` (alert-by-threadid fix).
- Uncommitted (the acquire fix + diagnostics):
  - `gpu_bridge_protocol.hpp` — `protocol_version` 16, acquire request carries semaphore + fence.
  - `vulkan_shim.cpp` — `vkAcquireNextImageKHR` forwards semaphore + fence.
  - `gpu_bridge.cpp` — acquire handler forwards them; first-frame milestone log.
  - `vulkan_host.{hpp,cpp}` — `acquire_next_image` signals semaphore/fence via empty submit.
  - `file.cpp` — `EMULATOR_LOG_READ` diagnostic.
  - `windows_emulator.cpp` — `EMULATOR_LOG_STACKS` diagnostic.
- In `open-iw5` (separate repo): `src/module/watchdog.cpp` (non-functional diagnostic) and `OIW5DIAG`
  `OutputDebugStringA` markers in `fastfiles.cpp` — both should be reverted before any open-iw5 commit.

---

## Update (2026-06-12): frames now present, but the menu is black — root cause = empty shader-constant buffer

The window now presents frames (the AFD non-blocking deadlock and many bridge gaps are fixed). The
menu renders **black**, and this was driven all the way to a precise root cause.

### What "black" actually is

`EMULATOR_LOG_PRESENT` reports the readback of the presented swapchain image. Black is exact, not
"dark": `present readback 1282x722: 925604/3702416 nonzero bytes`. `925604 == 1282*722` and
`px0=0xff000000`, i.e. **exactly one non-zero byte per pixel = the alpha byte (0xff)**; every RGB
component is zero. With `EMULATOR_CLEAR_GREEN` the readback is uniformly green
(`1851208 == 2*pixels` non-zero, all `0xff00ff00`) — proving **zero fragments** are produced: the
clear reaches the screen, the draws contribute nothing. This is a rasterization/geometry failure,
not shading.

### Ruled out (with evidence)

- **Vertices** are valid. `EMULATOR_DUMP_VTX` dumps the bound vertex buffer: real menu coordinates
  in a large 2D space, e.g. `(452.25, -300, 0, 1)`, `(225.4, 64.6, 0, 1)`. They require a transform.
- **Descriptor dynamic offsets** — forwarded now (commit `780604ab`), but DXVK doesn't even use
  `*_BUFFER_DYNAMIC` here (every `dynamicOffsetCount == 0`). Correct fix, not this bug.
- **Scissor / viewport / cull / rasterizer-discard** — all correct (full `1282x722` scissor + viewport,
  cull alternates NONE/BACK, discard never enabled). The UI pass has **no depth attachment**.
- **WHP direct-map coherency works.** A sentinel written by the guest through the aliased VA is read
  back correctly host-side **when probed at a high offset** DXVK isn't concurrently writing
  (`host_sees=0xCAFEBABE` for all 6 mapped memories). Probing offset 0 gave bogus values only because
  another vCPU/DXVK thread overwrites offset 0 during the host trap. The MMIO unit test only exercised
  guest-write coherency on Unicorn/Icicle (WHP skips the guest-CPU step), so this had never been
  validated on WHP — it is now.
- **Push constants** are correct: the UI fragment push block encodes the RT size (`1280`,`720` as int
  bit patterns).
- **`vkCmdUpdateBuffer`** is wired but DXVK records **zero** of them — constants don't come that way.
- **DMA constant buffers** (`d3d9.deviceLocalConstantBuffers`) — forcing it `False` via `dxvk.conf`
  (confirmed `Effective configuration: d3d9.deviceLocalConstantBuffers = False`) **did not change the
  symptom**. So the host-visible vs device-local copy path is not the cause.

### Root cause (localized)

The vertex shader reads its constant buffer through a descriptor that, in our bridge, points at an
**essentially-empty** memory, while DXVK's actual constant data lives in a **different** memory.

Concretely (`EMULATOR_DUMP_VTX` UBO dump + `MEMSCAN`, reading the *live* persistent host pointer the
GPU sees — not a re-map):

- The bound VS-constant descriptor (`set … bind 0`, `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`) resolves to
  memory `M_desc` at a per-draw streaming offset (`0x410000`, `0x410200`, …). Reading `M_desc` there
  is **zero**. `MEMSCAN` of `M_desc` (16 MB) finds **~15 non-zero dwords in the entire buffer**.
- Two other host-visible memories (4 MB each) are **densely populated** (`~28k`/`~36k` non-zero
  dwords) — that is where DXVK actually streamed the shader constants.

So the constants are written, but to a buffer the descriptor doesn't reference. The vertex shader
reads zeros → multiplies vertices by a zero matrix → all geometry collapses off-screen → zero
fragments → black. (`robustBufferAccess2` is enabled, so an out-of-range/empty read returns 0 rather
than crashing, which is consistent with the clean black.)

This is a **buffer↔memory binding / object-identity mismatch in the GPU bridge**, exposed by DXVK's
constant-buffer streaming with **orphaning/renaming** (`D3D9ConstantBuffer::Alloc` →
`DxvkContext::invalidateBuffer` + `allocateStorage`, see DXVK `src/d3d9/d3d9_constant_buffer.cpp`).
When the 1 MB slice fills, DXVK swaps the buffer's backing storage and rebinds the descriptor range;
our tracking of which `VkBuffer` is bound to which `VkDeviceMemory` (or the object-id reuse across
buffer destroy/recreate) ends up pointing the descriptor at stale/empty memory.

### Next step for a fresh session

Trace one constant buffer's full lifecycle by id: log every `vkCreateBuffer`,
`vkBindBufferMemory(buffer → memory @ offset)`, buffer destroy, and the descriptor's referenced
buffer over time. Confirm whether (a) the descriptor's `VkBuffer` is bound to the wrong memory in our
host, or (b) a destroyed buffer's object id is reused while our `buffer_data.memory_id` stays stale.
Then fix the binding/identity handling so the descriptor and DXVK's writes reference the same memory.

### Diagnostics added (all env-gated, kept per request)

- `vulkan_host.cpp`: `EMULATOR_DUMP_VTX` dumps bound vertex data, the bound UBO contents (via the live
  persistent host pointer, with an all-memory `MEMSCAN`), and push constants; `EMULATOR_LOG_BIND` now
  also logs `set_scissor` and `begin_rendering` (render area / loadOp / clear / attachment views).
- `gpu_bridge.cpp`: `handle_download_memory` reads the **live** host pointer for direct-mapped memory
  instead of re-mapping an already-mapped allocation (a real correctness fix); `EMULATOR_LOG_MAP`
  logs the memory id on direct-map success.
- `memory_data` now records the persistent host pointer + mapped size; `buffer_data` records its bound
  memory id/offset; `descriptor_set_data` records per-binding buffer infos — all used by the dumps.

---

## Update (2026-06-12, later): constants are NOT empty — correction + spec-constant fix

The "empty shader-constant buffer" conclusion above is **wrong** and is retracted. It was an artifact
of reading **offset 0** of the descriptor range. Reading the *live* persistent host pointer at the
right offset shows the constants are present and correct:

- The bound VS uniform (range 512) holds a valid 2D orthographic projection at **byte offset +256**:
  `[1/640,0,0,-1 / 0,-1/360,0,1 / 0,0,0,1 / 0,0,0,1]` (`1/640 = 0.0015625`, `-1/360 = -0.00277778`).
  Vertices like `(225,64)` map to `(-0.65, 0.82)` — **on screen**. So geometry transforms correctly.

So with vertices valid, the transform correct, and geometry landing on-screen, "zero fragments" must
mean **fragments are produced but write nothing** — and since blending is disabled with a full RGBA
write mask, the only remaining explanation is the **fragment shader discarding** (note: the
`EMULATOR_CLEAR_GREEN` "uniform green" result cannot distinguish "no fragments" from "all fragments
discarded" — both leave the clear untouched).

### Real bug fixed: specialization constants were dropped

DXVK bakes d3d9 render state into shaders via **`VkSpecializationInfo`** (see
`src/d3d9/d3d9_spec_constants.h`: `SpecAlphaCompareOp` is a 3-bit `VkCompareOp`, plus sampler types,
fog, FF texture-stage ops, …). The GPU bridge **dropped `pSpecializationInfo` entirely** — no handling
in the protocol, shim, or host — so every spec constant defaulted to 0. For the alpha test that means
`VK_COMPARE_OP_NEVER`, which discards every fragment.

Fix (this commit): forward per-stage specialization constants end-to-end —
`specialization_map_entry` wire struct + `vs/fs_spec_*` fields on `create_graphics_pipeline_request`
(protocol_version → 24); the shim marshals each stage's `pSpecializationInfo` (map entries + data);
the bridge parses the two trailing blocks; the host rebuilds a `VkSpecializationInfo` per stage and
attaches it. Verified working: the fragment stage now receives e.g. `entries=8 data=32`,
`fsdata=[1 0 1000104013 0 1000104013 1 0 0]` (real DXVK packed spec words), not zeros.

### Status: still black after the spec-constant fix

Forwarding spec constants is necessary and correct, but **did not by itself** make the menu render —
present readback is still alpha-only. So either alpha-test discard was not the (sole) cause, or the
menu's main draws use a different fragment shader/discard path. There are multiple FS pipelines
(`entries=8` and `entries=2` variants); which one the visible menu geometry uses is not yet pinned.

### Next step

Identify the fragment shader used by the visible menu draws and why it produces no color: decode the
forwarded spec-constant bitfield for that pipeline (confirm the alpha-test op resolves to a passing
value), and/or capture whether the SPIR-V actually declares spec constants with the forwarded
constant IDs (0..7) so the specialization takes effect (if the IDs don't match, the override is
silently ignored and the shader keeps its default-0 constants). Also re-confirm whether the visible
draws sample a texture whose result drives a discard.

---

## Update (2026-06-12, final for this session): root cause = empty SpecData UBO → alpha test NEVER

Pinned the black screen to a precise mechanism:

1. The fragment shader's alpha-test compare op is `SpecAlphaCompareOp` (DXVK spec constant, dword 1,
   bits 21..23 of the packed spec data; `src/d3d9/d3d9_spec_constants.h`).
2. The shader reads it via `Select(IsOptimized, specConstant, ubo)` — and `IsOptimized` defaults
   **false**, so for the menu pipelines it reads from the runtime **SpecData uniform buffer**
   (`CbvIndex::SpecData = 20`, remapped to an actual descriptor binding), not from
   `pSpecializationInfo`.
3. DXVK sets `alphaOp = VK_COMPARE_OP_ALWAYS (7)` when the alpha test is **disabled**
   (`d3d9_device.cpp`: `m_alphaTestEnabled ? DecodeCompareOp(...) : VK_COMPARE_OP_ALWAYS`).
4. In the bridge that descriptor (the menu set's **binding 1**, a 256-byte uniform buffer) reads
   **all zero** → `AlphaCompareOp = 0 = VK_COMPARE_OP_NEVER` → the alpha test always fails → **every
   fragment is discarded** → uniform clear color (black). The streaming VS-constant buffer (binding 0)
   *is* populated (the ortho matrix), so it is specifically the fixed/SpecData constant buffer that is
   empty.

`DSLAYOUT` confirms the menu set has only buffer bindings 0 and 1; the dense per-draw constant data
actually lives in *other* host-visible memories (`MEMSCAN`: two 4 MB allocations with ~28k/36k
non-zero dwords) while the descriptor-referenced 16 MB allocation is nearly empty — i.e. there is a
buffer↔memory association mismatch for the fixed constant buffers.

### Tried and inconclusive

Forwarding `pSpecializationInfo` is correct and committed, but does not help the menu pipelines
because they run with `IsOptimized = false` and read the UBO. An env-gated experiment that forced
`IsOptimized = true` + `AlphaCompareOp = ALWAYS` in the forwarded FS spec data did **not** turn the
menu visible and was reverted — most likely because the `IsOptimized` selector's spec-constant id in
the shipped **DXVK 2.7.1** differs from the cloned master (`DxvkLimits::MaxNumSpecConstants = 20`), so
the override never engaged. The clean fix is to make the SpecData/fixed constant buffer non-empty,
not to bypass it.

### Next step (concrete)

Trace the fixed (non-streaming) constant buffers — `Kind::PS/SpecData`, written once when render state
changes, `HOST_VISIBLE|HOST_COHERENT|HOST_CACHED` — through `vkCreateBuffer` → `vkBindBufferMemory` →
`vkMapMemory` → the descriptor write, by object id. Determine whether (a) DXVK's one-time write lands
in a different `VkDeviceMemory` than the descriptor references (binding/identity mismatch), or (b) the
`HOST_CACHED` mapping isn't reflecting the guest's writes in the bridge. Fixing that makes
`SpecAlphaCompareOp = ALWAYS` reach the shader and should unblock the first visible frame.
