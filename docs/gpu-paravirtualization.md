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

All verified end-to-end in the emulator against the host's real GPUs (an Intel UHD 630 + an Nvidia
Quadro P2000 on the dev machine). Each slice is one commit on `gpu-bridge-device`; release builds
clean, the `tidy` (clang-tidy) preset is clean, and `analyzer.exe -s test-sample.exe` smoke tests
pass throughout.

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

### M2 — submission, sync, and codegen (in progress)

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

## Remoted Vulkan entry points so far

Instance/device: `vkCreateInstance`, `vkDestroyInstance`, `vkEnumeratePhysicalDevices`,
`vkGetPhysicalDeviceProperties`, `vkGetPhysicalDeviceQueueFamilyProperties`, `vkCreateDevice`,
`vkDestroyDevice`, `vkGetDeviceQueue`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`
(+ `vk_icdGetInstanceProcAddr`).

Command/sync: `vkCreateCommandPool`, `vkDestroyCommandPool`, `vkAllocateCommandBuffers`,
`vkFreeCommandBuffers`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkCreateFence`,
`vkDestroyFence`, `vkResetFences`, `vkGetFenceStatus`, `vkQueueSubmit`, `vkWaitForFences`.

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
- The host driver is whatever `vulkan-1.dll` the host resolves (the dev machine has a real ICD;
  SwiftShader can be dropped in as a software fallback).

## What's left (plan)

Rough dependency order toward rendering a triangle offscreen against the host GPU and reading it
back:

1. **Generator: handle translation.** Emit `object_id`↔host-handle translation for handle members
   so structs that reference `VkInstance`/`VkBuffer`/... can be generated. This is the missing piece
   before most "real" structs.
2. **Generator: `pNext` sType-dispatch.** Walk and rebuild extension chains over an allowlist of
   extension structs.
3. **Migrate existing hand-marshalled commands to the generated path.** First win:
   `vkCreateInstance` honoring layers/extensions (e.g. validation layers) — a visible behavior
   improvement and the first real consumer of the codegen.
4. **Per-command marshalling generation** (the full Venus model), not just per-struct.
5. **Memory:** `vkAllocateMemory` + **zero-copy `vkMapMemory`** by mapping host GPU-visible memory
   into guest VA (the same mechanism Sogen uses for `KUSER_SHARED_DATA`).
6. **Resources + rendering:** buffers, images, image views, render pass, framebuffer, shader module,
   graphics pipeline, draw — enough for an offscreen triangle + readback.
7. **WSI/present:** wire `VkSwapchainKHR` present to the existing `ui_backend` / `present_surface`
   path (shares the window-presentation seam documented in `windows-ui-emulation.md`).
8. **Later:** OpenGL via Zink, DirectX via DXVK — no new bridge work, just guest DLL provisioning.

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
| `src/samples/vulkan-shim-test/` | Guest exe driving the shim through the real Vulkan API |
| `src/samples/gpu-bridge-probe-sample/` | Low-level probe (direct `DeviceIoControl`, no Vulkan headers) |
| `src/windows-emulator-test/vulkan_marshal_test.cpp` | Round-trip gtests for the generated marshalling |
| `deps/Vulkan-Headers` | Shallow submodule; `vulkan-headers` INTERFACE target |

Registration points: device name in `io_device.cpp` (`create_device`) and `syscalls/file.cpp`
(`get_io_device_name`); libraries in `src/CMakeLists.txt`; the `vulkan-headers` target in
`deps/CMakeLists.txt`.

The experimental SwiftShader **M0** samples (`vulkan-probe-sample`, `vulkan-triangle-sample`,
`vulkan-window-sample`, `README.vulkan.md`) are intentionally kept **untracked** — they validated
that DXVK/SwiftShader could run in-guest and will be overhauled, so they are deliberately not
committed.

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
logical device + queue, submits an empty command buffer, waits on a fence):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s vulkan-shim-test.exe vulkan-shim.dll"
```

Run the marshalling round-trip tests:

```cmd
cmd /c "cd build\release\artifacts && windows-emulator-test.exe --gtest_filter=VulkanMarshalTest.*"
```

Smoke tests (must stay green):

```cmd
cmd /c "cd build\release\artifacts && analyzer.exe -s test-sample.exe"
```
