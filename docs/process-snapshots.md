# Process Snapshots — Design

Status: in progress on branch `process-snapshots`

Goal: pause an emulated process on Android, write it to disk, and resume it later, including after
Android killed the `:sogen` process. Development happens on desktop WHP with one vCPU; the guest-side
code is shared with Android's single-vCPU FEX backend.

- [x] CPU/kernel snapshot from the sandbox: `--suspend-to FILE` saves on Ctrl+C, `--resume FILE`
      continues (fd08c0ab). Snapshot code moved from the analyzer into `windows-emulator`.
- [x] Save is transactional (temporary file, moved into place; a failed save keeps the process
      running), devices can refuse snapshots (the GPU bridge does until slice 0), the launch
      configuration is checked on resume, and snapshot options force one vCPU.
- [ ] GPU slice 0 — headless Vulkan round-trip (see Milestones)
- [ ] GPU slice 1 — swapchain, descriptors, graphics pipeline
- [ ] Child processes
- [ ] Android integration

## Decisions

- GPU state is in scope from the start; games are the main target.
- Child processes are snapshotted too: the parent asks each child sandbox to save itself.
- All resource contents are saved, including attachments, depth/stencil and MSAA images.
- GPU objects use typed, versioned records per Vulkan object type, rebuilt through `vulkan_host`
  helpers. Bridge IOCTLs are not replayed, since handlers have guest-visible side effects (guest VA
  allocation, output writes, window/surface creation).
- Steam is out of scope for now; the focus is the GPU.
- Suspend is a user action (a Suspend button), not backgrounding: Android gives no guaranteed time
  to write gigabytes after `onStop`.

## What exists

`windows_emulator::serialize` already covers registers, guest memory, modules, the syscall dispatcher,
registry runtime state and `process_context` (threads, all handle types, windows, GDI, input, ports,
timers). Files reopen by path, so the host setup must match: the launcher passes a description of its
configuration (emulation root, root mode, mounts, registry directory and files, network, GPU feature
spoofing), which is stored in the snapshot and compared on resume. Restore hooks re-create host
windows and reattach audio and console. Registry values imported from files at launch are part of the
saved registry overlay, so resume does not import them again.

Verified: a Python counter interrupted at count 12 (9.5 MB snapshot) resumed at 13 with its heap
intact; the guest clock jumped forward by the suspended time.

| Area | Today | Plan |
| --- | --- | --- |
| GPU bridge | Refuses snapshots | Typed records (below) |
| GPU memory mapped into the guest | Reservation metadata saved, contents skipped, reservation erased on restore | Bridge re-places device memory at the exact VA, saves the bytes itself |
| Imported guest memory | Saved with guest memory | Re-imported at the recorded address |
| Sockets (AFD) | Endpoint re-created, connection lost | Restored as reset connections |
| Child processes | Not saved | Nested snapshots |
| Multi-vCPU (WHP) | `serialize_state` throws | Save every vCPU; Android has one |
| Compatibility | No per-field versioning | Header with build id and executable size/mtime; mismatches discarded |

## GPU design

Vulkan cannot serialize objects. It only helps re-created objects match the originals: pipeline cache
data, shader module identifiers, and capture/replay addresses (buffer device address, descriptor
buffers). So every live object is re-created from its creation parameters under the bridge id the
guest already holds.

1. **Records.** Each `*_data` entry in `vulkan_host` keeps its creation parameters as owned copies.
   29 creation paths allocate ids today (`next_id++`).
2. **Forced ids.** Id allocation goes through one helper. During restore it hands out the recorded
   ids. Results are checked against the record; any mismatch fails the restore. Calls that create
   several ids (swapchain images, physical devices) record the full set.
3. **Order.** Instance, physical device, device, queues, memory, buffers and images, views and
   samplers, layouts, pipelines, descriptor pools and sets, command pools and buffers, sync objects.
4. **Tombstones.** An object destroyed while a live object still depends on its creation (shader
   module, set layout, render pass, pipeline layout, immutable sampler) keeps its record as a
   tombstone, created temporarily on restore and dropped with its last dependent. A final sweep
   runs at save.
5. **Mutable state is coalesced as it changes,** not journaled: memory bindings; the final contents
   of each descriptor slot (writes, copies, templates, pool reset/free); command buffer state
   (initial/recording/executable/invalid) plus the stream recorded since its last reset, moved out of
   `handle_record_commands` rather than copied; fence, semaphore and event state read at save.
6. **Side-effecting objects** (guest-mapped memory, imported memory, surface, swapchain) get
   dedicated restore code.

## Safe point and sync contract

Agreed with sol6_1 during design review.

- **Stop every guest thread first,** then check that no thread is inside an IOCTL.
- **The stop takes effect after the syscall completes.** Results and the syscall return must be
  committed before the CPU state is saved.
- **Games arm the suspend at present.** The stop lands after the next `queue_present` IOCTL has fully
  returned, which is a frame boundary for DXVK.
- **Check waits before draining.** `vkDeviceWaitIdle` deadlocks on a timeline wait whose signal has
  not been submitted. A shadow of submissions, acquires and presents decides whether every pending
  wait (binary and timeline) can be satisfied without more guest work. If not, resume and retry at
  the next safe point; reject after N tries.
- **Unconsumed binary signals** are re-created on restore by an empty submit that signals them.
- **Present completion.** Returning from present does not mean the presentation engine is done. Use
  present fences/present wait where available; otherwise treat restore as best-effort on the same
  device and surface, and fail when safe teardown cannot be shown.
- **Swapchain (v1).** The real swapchain is re-created with the recorded parameters. Restore requires
  the same image count and zero acquired images, and assumes the guest redraws every frame (re-created
  images have undefined contents). A virtual swapchain is a later option if Android surface loss or
  count mismatches require it; it is a larger WSI implementation (own acquire/present state,
  semaphores, old-swapchain retirement, TRANSFER_DST support check, scaling).
- **Transactional.** A failed save discards its temporary file and resumes the original process. A
  failed restore reports an incompatible or incomplete snapshot and never modifies the saved file.

## Resource contents and layouts

- **Readback is capability-checked per resource.** Transfer usage is not blindly OR'd into creation
  usage: formats and surfaces constrain it, and it can cost compression on some drivers. Resources
  without a supported path are rejected explicitly.
- **Buffers** by copy; **non-MSAA color images** per mip/layer; **depth/stencil** per aspect;
  **MSAA** via shader passes (last).
- **Layouts** are tracked per aspect, mip and layer, plus queue-family ownership, from explicit
  barriers and render-pass initial/final transitions. Per-command-buffer effects are applied at
  submit in submission order (one queue for now).
- **Buffer device address** needs the full capture/replay chain from device creation (feature,
  buffer create flag + opaque capture address, memory allocation flag + opaque address). The bridge
  currently drops buffer creation flags. Excluded until implemented.
- **Queries**: completed results cannot be injected into a new pool. Live query pools are rejected
  until a shadow of results exists.
- **External and sparse resources** are rejected.

## Milestones

Slice 0 — headless, one guest queue, one submission thread:

- Instance, physical device, device, queue, memory (allocate and direct map), buffer, non-MSAA color
  image, command pool/buffer, fence, semaphore.
- Unsupported live object types reject the snapshot.
- Test sample `vulkan-snapshot-test`: device-local buffer and image whose contents evolve every
  iteration, a persistent mapped buffer, and a printed checksum sequence. Ctrl+C mid-loop and resume
  must match an uninterrupted baseline, including the first iteration after resume and the exact VA
  and bytes of the persistent mapping.

Slice 1:

- Swapchain and the present-armed safe point.
- Descriptor set layout, pool and sets with coalesced slot state.
- One graphics pipeline.
- Tests for a resubmitted command buffer, descriptor overwrite and copy, pool reset, and a mapped
  write made before the snapshot.

Then Settlers II (32-bit D3D9 through DXVK, WHP): suspend in game and resume.

Later: more object types, MSAA, queries, BDA, multiple queues, multi-vCPU WHP, child processes, the
Android Suspend/Resume UI.

## Performance

Normal running must not get slower. Records are written at create time only. The hot paths are
descriptor updates (about 92% of GPU IOCTLs in MW2), coalesced into a per-slot table update, and
command streams, kept by moving the existing vector. Layout tracking costs a map update per barrier.
All draining and readback happens only at suspend. Measured against MW2 and Baba Is You fps before and
after.

## Side findings

- Python 3.14 `time.sleep` fails with WinError 50: the high-resolution waitable timer it uses is not
  implemented.
- Main now needs `apisetschema.dll` in the emulation root (#1577).
