# GPU paravirtualization status

Tracking issue: [#1032](https://github.com/momo5502/sogen/issues/1032). Work lives on branch
`gpu-bridge-device`.

## Goal

Give guest binaries real GPU graphics support so applications that render can run and be analyzed
in the emulator. The technique is **GPU API remoting** (a.k.a. API paravirtualization) at the
**Vulkan** boundary: the guest's Vulkan calls are forwarded across the emulator boundary and
executed against the **host's real Vulkan driver**. We never emulate a GPU device or parse
proprietary command buffers — the part that makes "real" GPU virtualization infeasible.

This is the same approach as Mesa's **Venus** (Vulkan-over-virtio-gpu) and **virglrenderer**.

## Why this fits a user-space emulator

The Windows graphics stack has two vendor components: a **user-mode driver / ICD** (e.g.
`nvoglv64.dll`) and a **kernel-mode driver** behind `dxgkrnl.sys` that builds vendor-proprietary
command buffers. Because Sogen emulates only **user space** (no kernel, no `dxgkrnl`, no hardware),
we replace the UMD/ICD with a **remoting shim** and skip the entire kernel/KMD/command-buffer layer.

DirectX/OpenGL can layer on top later (DXVK for D3D→Vulkan, Zink for GL→Vulkan) without new bridge
work. **DXVK is currently deprioritized** — the focus is running native Vulkan apps first.

## Architecture

```
guest app  (or, later, DXVK d3d11.dll / dxgi.dll)
  -> guest vulkan-1.dll shim            (serialize Vulkan calls)            src/samples/vulkan-shim/
    -> CreateFile(\\.\SogenGpu) + DeviceIoControl   [emulator boundary]
      -> SogenGpu io_device              (dispatch IOCTL -> command)        devices/gpu_bridge.cpp
        -> vulkan_host                   (real vulkan-1.dll / libvulkan)    devices/vulkan_host.cpp
          -> host GPU
      <- opaque object ids / results / fence status
```

### Key decision: a virtual driver device, not a custom syscall

Issue #1032's original sketch used a custom high syscall id. We instead remote over a **virtual
driver device** — the guest opens `\\.\SogenGpu` (NT path `\Device\SogenGpu`) and talks to it with
`NtDeviceIoControlFile`. This reuses Sogen's existing `io_device` infrastructure and gets, for free,
everything the syscall route would have had to reinvent:

| Concern | Driver device gives us |
| --- | --- |
| Dispatch plumbing | `NtCreateFile` → `io_device_container` → `NtDeviceIoControlFile` → `io_control()` already exists |
| Guest shim | plain `CreateFile` + `DeviceIoControl`; **no asm syscall stub**; works in host-DLL **and** root mode |
| Command + buffers | the IOCTL code is the command; input/output buffers are built in |
| Poll-and-yield | `STATUS_PENDING` + `work()` already used by the AFD socket device (`devices/afd_endpoint.cpp`) |
| Snapshots | `io_device::serialize_object` hook already in the device container |
| Analysis hooks | `callbacks.on_ioctrl(device, name, code)` fires automatically |

The blueprint for the harder device behaviors (async, poll-and-yield) is the existing
`devices/afd_endpoint.cpp` (sockets), which already returns `STATUS_PENDING` and completes work from
`work()` driven by the emulator main loop.

### Opaque object ids

Raw host pointers never cross the boundary. `vulkan_host` keeps the real `VkInstance` /
`VkPhysicalDevice` / `VkDevice` / ... in tables and hands the guest an opaque `object_id` (a
monotonic `uint64`). Because Vulkan handles are pointer-sized, the **guest shim stores the
`object_id` directly in the `VkInstance`/etc. handle value** — so the shim needs no handle table of
its own; the value the app holds *is* the id the host looks up.

### Wire protocol

`src/gpu-bridge-protocol/gpu_bridge_protocol.hpp` is a dependency-free header (only `<cstdint>`),
the single source of truth shared by host and guest:

- IOCTL codes via the standard `CTL_CODE` encoding (FILE_DEVICE_UNKNOWN, METHOD_BUFFERED), one per
  command, in the vendor function range (`0x800+`).
- Fixed-size request/response structs per command (`object_id`s, counts, `VkResult` as `int32`).
- Native Vulkan structs that have no pointers (e.g. `VkPhysicalDeviceProperties`,
  `VkQueueFamilyProperties`) ride the wire as **raw bytes** — both sides agree on the layout via
  their own `<vulkan/vulkan_core.h>`. This keeps the protocol header free of `vulkan.h`.

For **pointer-rich** structs (pNext chains, string arrays, nested structs) raw bytes are not enough,
so those are handled by generated marshalling (see below).

## Status — done

All verified end-to-end against a real Vulkan driver — the dev machine's real GPUs (an Intel UHD 630 +
an Nvidia Quadro P2000) and, on a GPU-less box, **SwiftShader** dropped in as a software ICD. The
windowed samples additionally run as **native** Vulkan apps against a real GPU (loading `vulkan-1.dll`
instead of the shim). Each slice is one commit (or merge) on `gpu-bridge-device`; release builds clean,
the `tidy` (clang-tidy) preset is clean, and `analyzer.exe -s test-sample.exe` smoke tests pass
throughout.

### M1 — minimal remoting (complete)

Issue #1032's M1 target was `vkCreateInstance → vkEnumeratePhysicalDevices → vkCreateDevice`. Done,
plus the surrounding calls needed to actually use them:

- **`3fa8096c` — bridge boundary.** The `SogenGpu` `io_device`; registered in `create_device` /
  `get_io_device_name` so `\\.\SogenGpu` resolves. A `GET_VERSION` handshake (magic + protocol
  version). Guest `gpu-bridge-probe-sample` does the handshake over `DeviceIoControl`.
- **`bc1e2586` — instance + physical-device queries.** Added `Vulkan-Headers` as a shallow `deps/`
  submodule exposed via a light `vulkan-headers` INTERFACE target. `vulkan_host` dynamically loads
  the host loader (`vulkan-1.dll` / `libvulkan.so`) and remotes `vkCreateInstance`,
  `vkEnumeratePhysicalDevices`, `vkGetPhysicalDeviceProperties`, `vkDestroyInstance`. The probe
  enumerates the real host GPUs.
- **`8654f8ce` — guest `vulkan-1.dll` shim.** `src/samples/vulkan-shim/` exports the Vulkan entry
  points and forwards them to the bridge; apps load it via `LoadLibrary` + `vkGetInstanceProcAddr`
  exactly like a real ICD. `vulkan-shim-test` drives it through the genuine Vulkan dance.
- **`1fe7e75d` — logical device + queues.** `vkGetPhysicalDeviceQueueFamilyProperties`,
  `vkCreateDevice`, `vkGetDeviceQueue`, `vkDestroyDevice`. A real `VkDevice` + graphics queue are
  created on the host GPU.
- **`053944d1`** — clang-tidy cleanup for the above.

### M2 — submission, sync, resources, and presentation (in progress)

- **`08cde6de` — command submission + fence sync (poll-and-yield).** Command pool/buffer + fence
  object model, `vkQueueSubmit`, and the blocking-call solution: the host endpoint only ever does a
  **non-blocking `vkGetFenceStatus`**, and the guest shim's `vkWaitForFences` spins on that poll
  while yielding to the emulator (`SwitchToThread` → `NtYieldExecution`). The real GPU progresses
  independently of the emulator thread, so the fence eventually signals — the single-threaded
  emulator is never blocked on the GPU. Verified: an empty command buffer is recorded, submitted
  with a fence, and awaited (`vkQueueSubmit` and `vkWaitForFences` both return `VK_SUCCESS`).

- **`59103a59` — vk.xml marshalling generator (codegen foundation).** Hand-marshalling does not
  scale to Vulkan's pointer-rich structs, so this introduces a Venus-style generator:
  - `tools/vulkan-bridge-generator/generate.py` parses `vk.xml` and emits concrete `encode`/`decode`
    overloads for an allowlist of structs into a checked-in header.
  - `src/vulkan-bridge-marshal/` is the runtime: `writer`/`reader` over a byte stream plus an
    `arena` that owns decoded pointees (so the host can pass rebuilt structs to the real driver).
    `encode` (guest) flattens; `decode` (host) rebuilds; the two are symmetric from one generated
    header so they cannot drift.
  - The `vulkan-bridge-generate` CMake target regenerates offline (Python is needed only to
    *regenerate*, never to build); verified it reproduces the checked-in header byte-for-byte.
  - Round-trip gtests (`src/windows-emulator-test/vulkan_marshal_test.cpp`) cover scalars,
    null-terminated strings, `len="count,null-terminated"` string arrays, and nested struct
    pointers, on `VkApplicationInfo` and `VkInstanceCreateInfo`.

- **device memory + host-visible buffers + readback.** The first slice that has the GPU *produce
  data the guest reads back* — the foundation for all rendering. Adds `vkGetPhysicalDeviceMemoryProperties`,
  `vkAllocateMemory`/`vkFreeMemory`, `vkCreateBuffer`/`vkDestroyBuffer` +
  `vkGetBufferMemoryRequirements`/`vkBindBufferMemory`, a real recorded command (`vkCmdFillBuffer`),
  and memory readback. Because `VkDeviceMemory` is host-side, the guest's `vkMapMemory`/`vkUnmapMemory`
  are emulated by **staging a guest-side copy**: `download_memory` fills the staging buffer on map and
  `upload_memory` flushes it back on unmap, so a host pointer never crosses the bridge (zero-copy
  mapping is a later optimization). Verified end-to-end: the guest allocates a host-visible buffer,
  the GPU fills it via `vkCmdFillBuffer`, and the mapped readback returns the exact pattern
  (`vulkan-shim-test`'s `fill+readback` check).

- **images + clear + image→buffer readback.** The offscreen render-target + readback path that
  windowed present will reuse. Adds `vkCreateImage`/`vkDestroyImage` +
  `vkGetImageMemoryRequirements`/`vkBindImageMemory`, image layout transitions
  (`vkCmdPipelineBarrier`, image memory barriers only), `vkCmdClearColorImage`, and
  `vkCmdCopyImageToBuffer`. Verified end-to-end: the GPU clears a 16×16 `R8G8B8A8_UNORM` image to a
  known color, transitions it `UNDEFINED → TRANSFER_DST → TRANSFER_SRC`, copies it into a host-visible
  buffer, and the readback matches every pixel (`vulkan-shim-test`'s `clear+readback` check).

- **WSI: windowed presentation to a guest window.** The first *visible* milestone — a guest Vulkan app
  that opens a real Win32 window and shows its GPU-rendered content. Adds `vkCreateWin32SurfaceKHR`,
  `vkDestroySurfaceKHR`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`,
  `vkAcquireNextImageKHR`, and `vkQueuePresentKHR`. There is **no real host WSI**: a surface is just the
  guest HWND, and a swapchain is N offscreen images plus a host-visible readback buffer. The guest
  stays a faithful WSI app (it transitions to `PRESENT_SRC_KHR`, which the host maps to
  `TRANSFER_SRC_OPTIMAL` so the real driver stays valid). On `vkQueuePresentKHR` the host copies the
  presented image into the readback buffer, waits, and the bridge hands the BGRA pixels to the guest
  window through `win_emu.ui().present_surface(hwnd, …)` — the same UI-backend seam GDI `EndPaint`
  uses. New guest sample `vulkan-window-sample` creates a 320×240 window, clears the swapchain image to
  an animated color each frame, and presents; the window shows the live GPU output (verified on screen
  against SwiftShader).

- **Graphics pipeline: a triangle in the window.** The first *real rendering* (not just transfer
  clears). Adds `vkCreateShaderModule`, `vkCreateImageView`, `vkCreateRenderPass`, `vkCreateFramebuffer`,
  `vkCreatePipelineLayout`, `vkCreateGraphicsPipelines` (+ destroys), and the draw recording
  `vkCmdBeginRenderPass` / `vkCmdBindPipeline` / `vkCmdDraw` / `vkCmdEndRenderPass`. The host hardcodes
  most fixed-function state (no vertex input, triangle list, static full-extent viewport/scissor, one
  non-blended color attachment); the guest supplies the SPIR-V, render pass attachment, and viewport
  extent. `vulkan-window-sample` now renders the classic RGB-gradient triangle through a render pass
  into the swapchain image and presents it — verified on screen against SwiftShader. Shader SPIR-V is
  compiled offline (glslang) and checked in as `triangle_spirv.hpp`, so the build needs no shader
  compiler.

- **Spinning-cube sample + host-vs-emulated FPS benchmark.** A slightly richer sample exercising the
  existing entry points harder: a solid 3D cube (6 faces, 36 baked-in vertices) transformed by a 64-byte
  mat4 MVP push constant computed on the CPU. With no depth attachment and no culling in the bridge, the
  six faces are CPU-sorted back-to-front each frame (painter's algorithm) and drawn with per-face
  `vkCmdDraw` calls (`firstVertex = face*6`). The rotation is driven by real wall-clock time (not the
  frame count) so it spins at the same physical rate natively and emulated, and the loop prints FPS to
  stdout once per real second so the same binary's throughput can be compared through the shim vs against
  a real `vulkan-1.dll`. No new bridge/shim work was needed.

- **Push constants + a spinning-triangle sample.** Adds `vkCmdPushConstants` and a push-constant range
  on the pipeline layout — enough for a shader to read small per-frame data without a vertex/uniform
  buffer. New guest sample `vulkan-spinning-triangle-sample` rotates the triangle by a push-constant
  angle and shows its frame rate in the window title. The rotation angle advances per *frame* (not per
  wall-clock second) because the emulated tick counter does not track real time across host-side blocks,
  and the FPS is measured from the system time (`GetSystemTimeAsFileTime`) rather than `GetTickCount`.

- **Running the samples on a real driver (not just the bridge).** Each Vulkan sample doubles as an
  ordinary native Vulkan app: load the real `vulkan-1.dll` instead of the shim (`… .exe vulkan-1.dll`)
  and it runs against a real GPU. This is a strong correctness check on the shim's API fidelity — the
  *same* binary runs through the bridge and against a real loader — and it surfaced (and fixed) several
  real-WSI requirements the bridge had masked:
  - **`VK_SUBOPTIMAL_KHR`** is a success code real drivers return from acquire/present; the samples treat
    it as non-fatal instead of bailing.
  - **Per-frame fence sync** — each frame waits on a fence before presenting, instead of relying on the
    bridge's implicit `vkQueueWaitIdle` (a real driver does not serialize frames for you).
  - **Swapchain extent matching** — `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` is remoted, and the
    samples size the swapchain/viewport/framebuffers/render-area to `caps.currentExtent` (the surface's
    client area in *physical* pixels). Hardcoding the window size made a real driver reject the present
    with `VK_ERROR_OUT_OF_DATE_KHR` (client area < window, possibly DPI-scaled). The bridge returns an
    undefined `currentExtent`, in which case the guest falls back to the window client rect. The sample
    windows are fixed-size, since the swapchain is not recreated on resize.
  - The spinning sample presents with `VK_PRESENT_MODE_IMMEDIATE_KHR` (no vsync) so its FPS reflects real
    throughput rather than the refresh rate (the bridge ignores the present mode).

  > Building the FPS readout also turned up an unrelated emulator bug: `GetTickCount[64]` advanced
  > ~10000× too fast (`kusd_mmio::update()` derived the tick count from 100 ns units where the guest
  > expects milliseconds). Fixed separately in [#1037](https://github.com/momo5502/sogen/pull/1037).

## Remoted Vulkan entry points so far

Instance/device: `vkCreateInstance`, `vkDestroyInstance`, `vkEnumeratePhysicalDevices`,
`vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkCreateDevice`,
`vkDestroyDevice`, `vkGetDeviceQueue`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`
(+ `vk_icdGetInstanceProcAddr`).

Command/sync: `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkAllocateCommandBuffers`,
`vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkCreateFence`,
`vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkQueueSubmit`, `vkWaitForFences`.

Memory/buffers: `vkGetPhysicalDeviceMemoryProperties`, `vkAllocateMemory`, `vkFreeMemory`,
`vkMapMemory`, `vkUnmapMemory`, `vkCreateBuffer`, `vkDestroyBuffer`, `vkGetBufferMemoryRequirements`,
`vkBindBufferMemory`, `vkCmdFillBuffer`.

Images/transfer: `vkCreateImage`, `vkDestroyImage`, `vkGetImageMemoryRequirements`,
`vkBindImageMemory`, `vkCmdPipelineBarrier`, `vkCmdClearColorImage`, `vkCmdCopyImageToBuffer`.

WSI: `vkCreateWin32SurfaceKHR`, `vkDestroySurfaceKHR`, `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`,
`vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`, `vkGetSwapchainImagesKHR`, `vkAcquireNextImageKHR`,
`vkQueuePresentKHR`.

Graphics pipeline: `vkCreateShaderModule`, `vkCreateImageView`, `vkCreateRenderPass`,
`vkCreateFramebuffer`, `vkCreatePipelineLayout` (with a push-constant range), `vkCreateGraphicsPipelines`
(+ destroys), `vkCmdBeginRenderPass`, `vkCmdBindPipeline`, `vkCmdPushConstants`, `vkCmdDraw`,
`vkCmdEndRenderPass`.

The hand-written entry points currently pass minimal/empty create-infos (e.g. `vkCreateInstance`
ignores layers/extensions, `vkQueueSubmit` ignores semaphores). The generator is the path to
honoring the full structs.

## Known limitations / simplifications

- **No snapshot support for live GPU state.** `gpu_bridge_device` serializes as a no-op; host Vulkan
  objects can't be serialized. Restoring a snapshot taken with an open GPU handle is unsupported
  (experimental).
- **`pNext` chains are dropped** by the generated marshalling (encode writes "no chain", decode
  yields `nullptr`). Extension structs need sType-dispatched chain walking.
- **Handle members inside marshalled structs aren't handled yet** — they need the guest↔host
  `object_id` translation hook. The current allowlist (`VkApplicationInfo`, `VkInstanceCreateInfo`)
  has none.
- The generator does not yet handle: arrays of non-string structs, unions, fixed inline arrays.
- **`vkQueueSubmit`** remotes a single command buffer per submission with no wait/signal semaphores
  (the fence is attached to the final command buffer).
- **`vkCreateDevice`** creates a single queue family from the app's first `VkDeviceQueueCreateInfo`,
  no device extensions/features.
- **`vkMapMemory` is a staged copy, not zero-copy.** On map the host range is downloaded into a
  guest-side buffer; on unmap it is uploaded back. Correct for read/write, but a host↔guest copy per
  map. Zero-copy (mapping host GPU-visible memory into guest VA) is a later optimization.
- **Presentation is readback-based (no real swapchain), now asynchronous (one frame behind).** Each
  `vkQueuePresentKHR` records an image→buffer copy into a host-visible readback buffer; the bridge then
  hands those pixels to the UI backend. No real present semaphores/acquire fences are modeled
  (`vkAcquireNextImageKHR` ignores them and hands out images round-robin) and
  `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` is mapped to `TRANSFER_SRC_OPTIMAL` on the host.
  - The copy is **submitted but not waited on inline**: it runs on the GPU while the guest renders the
    next frame, and the *next* present drains it (its fence is essentially always already signalled by
    then). So present is one frame behind (imperceptible) and the host thread never blocks on
    `vkQueueWaitIdle`. The readback buffer is allocated `HOST_CACHED` (not just `HOST_COHERENT`) so the
    ~920 KB CPU read back is an order of magnitude faster.
  - *Measured (WHP/Hyper-V backend, dev machine, `vulkan-cube-sample`'s built-in per-phase profiler).*
    The synchronous readback present originally dominated the emulated frame: ~3.4 ms/frame (~290 FPS),
    of which present was ~2.7 ms (≈79%) — split host-side into ~0.88 ms `vkQueueWaitIdle` + ~1.25 ms of
    920 KB readback memcpy (the buffer was uncached) + ~0.45 ms SDL upload. Two fixes: `HOST_CACHED`
    readback cut the memcpy ~1.25 ms → ~0.065 ms (→ ~450 FPS), and async present removed the inline
    `vkQueueWaitIdle` (→ ~650 FPS, ~1.54 ms/frame). Net **~290 → ~650 FPS (~2.2×)** vs ~0.21 ms native.
    The boundary cost is real but secondary at this command count (each remoted call is ~7.5 µs under
    WHP); it is now also **batched** — a whole command buffer's recording crosses in one IOCTL (see
    command batching below), so the cube's recording dropped from ~0.18 ms (12 IOCTLs) to ~0.06 ms (1).
    Remaining big phases are now the guest's per-frame fence wait (~0.6 ms, poll-and-yield) and the SDL
    `present_surface` upload (~0.45 ms); eliminating the per-frame GPU→CPU→GPU round-trip entirely
    (zero-copy / GPU-side present) is the next lever.
- **Command recording is batched (`ioctl_record_commands`).** Command-buffer recording commands
  (`vkBeginCommandBuffer`, the `vkCmd*` calls, `vkEndCommandBuffer`) are no longer one IOCTL each. The
  guest shim appends each to a per-command-buffer byte stream (a `command_record_header` + that command's
  normal request struct as payload) and flushes the whole `begin → cmds → end` stream to the bridge in a
  **single** IOCTL at `vkEndCommandBuffer`; the host replays it (`execute_recorded_command`). This makes
  recording cost ~O(1) boundary crossings regardless of command count — essential for real apps that
  issue hundreds/thousands of draws per frame (where one-VM-exit-per-command would dominate). Measured at
  600 draws/frame: recording stays ~0.15 ms batched vs an estimated ~4.5 ms unbatched (600 × ~7.5 µs).
  The per-command IOCTLs and their host handlers were removed; the request structs remain as the stream
  payload format.
- **Surface capabilities are synthetic and the swapchain is never recreated.**
  `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` returns fixed caps with an *undefined* `currentExtent`
  (the guest chooses the extent) and permissive min/max; `vkCreateSwapchainKHR`'s requested present mode
  and pre-transform are not really applied (the readback path is the only present path). The bridge does
  not implement swapchain recreation, so `VK_ERROR_OUT_OF_DATE_KHR`/resize is not handled — the sample
  windows are fixed-size to avoid it. Surface-format and present-mode *enumeration* are not remoted
  either; the samples just pass valid values directly (`B8G8R8A8_UNORM`, FIFO/immediate).
- The host driver is whatever `vulkan-1.dll` the host resolves. The dev machine has a real ICD; on a
  GPU-less box (e.g. CI) SwiftShader can be dropped in as a software fallback by staging its loader +
  ICD next to `analyzer.exe` and pointing `VK_ICD_FILENAMES` at the manifest. Software drivers
  JIT-compile their submit path on the first `vkQueueSubmit`, so the first `vkWaitForFences` needs a
  generous timeout.

## What's left (plan)

The near-term goal — a **windowed** Vulkan sample that opens a real window and shows its GPU-rendered
content — is **reached**: `vulkan-window-sample` renders a triangle through a graphics pipeline into a
guest window, and `vulkan-spinning-triangle-sample` adds a push-constant-rotated triangle with an FPS
readout (steps 1–4 below all done). Both samples also run as ordinary native Vulkan apps against a real
GPU (load `vulkan-1.dll` instead of the shim), which validates the shim's API fidelity. The approach is
**readback-and-present** — render on the host GPU into an
offscreen image, read the pixels back, and hand them to the guest window's surface through Sogen's
existing `present_surface` seam (the SDL backend already displays a per-HWND BGRA surface; see
`windows-ui-emulation.md`). The guest stays a real Vulkan WSI app (`vkCreateWin32SurfaceKHR` /
`vkCreateSwapchainKHR` / `vkQueuePresentKHR`), but the bridge implements the "swapchain" as plain
offscreen `VkImage`s. This avoids zero-copy memory mapping and avoids binding the host GPU to the SDL
window, and means a software driver without Win32 WSI (SwiftShader) still works.

Staged steps toward that:

1. **Memory + readback foundation** — *done* (host-visible buffers, `vkCmdFillBuffer`, map readback).
2. **Render target + clear** — *done* (`VkImage` + memory, layout barriers, `vkCmdClearColorImage`,
   `vkCmdCopyImageToBuffer` readback verified against a known clear color).
3. **WSI → first visible window** — *done* (`vkCreateWin32SurfaceKHR` + swapchain + `vkQueuePresentKHR`;
   present = readback + `present_surface(hwnd, …)`; `vulkan-window-sample` shows an animated color on screen).
4. **Triangle** — *done* (render pass, framebuffer, SPIR-V shader modules, graphics pipeline, `vkCmdDraw`;
   `vulkan-window-sample` renders the RGB-gradient triangle into the window).

Cross-cutting (needed as the create-infos get richer, can land alongside the steps above):

- **Generator: handle translation** — `object_id`↔host-handle translation for handle members, so
  pointer-rich structs can be generated instead of hand-marshalled.
- **Generator: `pNext` sType-dispatch** — walk/rebuild extension chains over an allowlist.
- **Migrate hand-marshalled commands to the generated path**, then **per-command generation** (the
  full Venus model).
- **Zero-copy `vkMapMemory`** — map host GPU-visible memory into guest VA (the mechanism Sogen uses
  for `KUSER_SHARED_DATA`), replacing the staged-copy map.
- **Fuller WSI** — swapchain recreation on `VK_ERROR_OUT_OF_DATE_KHR`/resize, real present and acquire
  semaphores, and surface-format/present-mode enumeration. (Today the samples are fixed-size and pass
  valid swapchain parameters directly.)
- **More draw plumbing** — vertex/index/uniform buffers and descriptor sets, then textures; these are
  the remaining pieces before non-trivial real apps render.

Later: OpenGL via Zink, DirectX via DXVK — no new bridge work, just guest DLL provisioning.

## File map

| Path | Role |
| --- | --- |
| `src/gpu-bridge-protocol/gpu_bridge_protocol.hpp` | Dependency-free wire protocol (IOCTL codes, request/response structs) |
| `src/windows-emulator/devices/gpu_bridge.{hpp,cpp}` | `SogenGpu` io_device; IOCTL → command dispatch + marshalling |
| `src/windows-emulator/devices/vulkan_host.{hpp,cpp}` | Emulator-free wrapper over the real driver; object-id tables. Kept in its own TU so host `<vulkan/vulkan_core.h>` + `<Windows.h>` don't clash with the emulated Windows types |
| `src/vulkan-bridge-marshal/vk_bridge_serial.hpp` | Serializer runtime: `writer` / `reader` / `arena` + string/array helpers |
| `src/vulkan-bridge-marshal/vk_bridge_marshal.generated.hpp` | **Generated** encode/decode (do not edit by hand) |
| `tools/vulkan-bridge-generator/generate.py` | The generator (parses `vk.xml`); `STRUCT_ALLOWLIST` controls coverage |
| `src/samples/vulkan-shim/` | Guest `vulkan-1.dll` shim (the deliverable) |
| `src/samples/vulkan-shim-test/` | Headless guest exe driving the shim (instance→device→fill/clear readback) |
| `src/samples/vulkan-window-sample/` | Windowed guest exe: Win32 window + swapchain + render-pass triangle. `triangle.{vert,frag}` are the GLSL sources; `triangle_spirv.hpp` is the checked-in compiled SPIR-V. Also runs natively against a real GPU (`… vulkan-1.dll`) |
| `src/samples/vulkan-spinning-triangle-sample/` | Windowed guest exe: a push-constant-rotated triangle with an FPS readout in the title bar. `spinning.{vert,frag}` + checked-in `spinning_triangle_spirv.hpp`. Also runs natively |
| `src/samples/vulkan-cube-sample/` | Windowed guest exe: a 3D spinning cube (mat4 MVP push constant; faces CPU-sorted back-to-front since the bridge has no depth/cull) that prints FPS to stdout every real second for host-vs-emulated comparison. `cube.{vert,frag}` + checked-in `cube_spirv.hpp`. Also runs natively |
| `src/samples/gpu-bridge-probe-sample/` | Low-level probe (direct `DeviceIoControl`, no Vulkan headers) |
| `src/windows-emulator-test/vulkan_marshal_test.cpp` | Round-trip gtests for the generated marshalling |
| `deps/Vulkan-Headers` | Shallow submodule; `vulkan-headers` INTERFACE target |

Registration points: device name in `io_device.cpp` (`create_device`) and `syscalls/file.cpp`
(`get_io_device_name`); libraries in `src/CMakeLists.txt`; the `vulkan-headers` target in
`deps/CMakeLists.txt`.

The experimental SwiftShader **M0** samples (`vulkan-probe-sample`, `vulkan-triangle-sample`,
`README.vulkan.md`) are intentionally kept **untracked** — they validated that DXVK/SwiftShader could
run in-guest and will be overhauled, so they are deliberately not committed. (The tracked
`vulkan-window-sample` above is the bridge's own WSI sample, unrelated to those.)

## Build, regenerate, validate

Build (release):

```cmd
cmake --build --preset=release
```

Regenerate the marshalling from `vk.xml` (only after changing `STRUCT_ALLOWLIST` or bumping the
`Vulkan-Headers` submodule):

```cmd
cmake --build --preset=release --target vulkan-bridge-generate
```

Run the bridge probe (low-level handshake + instance/device enumeration over `DeviceIoControl`):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s gpu-bridge-probe-sample.exe"
```

Run the shim test (guest app → `vulkan-shim.dll` → bridge → host GPU; enumerates devices, creates a
logical device + queue, submits a command buffer, and verifies `fill+readback` / `clear+readback`):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s vulkan-shim-test.exe vulkan-shim.dll"
```

Run the windowed samples in the emulator (each opens a guest window and presents the GPU output):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s vulkan-window-sample.exe vulkan-shim.dll"
cmd /c "cd build\release\artifacts && analyzer.exe -s vulkan-spinning-triangle-sample.exe vulkan-shim.dll"
cmd /c "cd build\release\artifacts && analyzer.exe -s vulkan-cube-sample.exe vulkan-shim.dll"
```

`vulkan-cube-sample` runs until its window is closed and prints the measured FPS to stdout once per
real second; an optional trailing argument caps the run length in seconds
(`vulkan-cube-sample.exe vulkan-shim.dll 10`). Because the analyzer advances the guest clock at real
wall-clock rate by default, the same binary run through the shim and against `vulkan-1.dll` yields
directly comparable FPS (emulated is ~15-20x slower on the dev machine).

The same sample exes also run as ordinary **native** Vulkan apps against a real GPU — pass the real
loader instead of the shim (this validates the shim's API fidelity, since the identical binary runs
both ways):

```cmd
cmd /c "cd build\release\artifacts && vulkan-spinning-triangle-sample.exe vulkan-1.dll"
```

On a GPU-less box (e.g. CI) drop in **SwiftShader** as a software ICD: stage `vulkan-1.dll`,
`vk_swiftshader.dll` and `vk_swiftshader_icd.json` next to `analyzer.exe` and point the loader at the
manifest before running:

```cmd
set VK_ICD_FILENAMES=%CD%\vk_swiftshader_icd.json
```

Run the marshalling round-trip tests:

```cmd
cmd /c "cd build\release\artifacts && windows-emulator-test.exe --gtest_filter=VulkanMarshalTest.*"
```

Smoke tests (must stay green):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s test-sample.exe"
```
