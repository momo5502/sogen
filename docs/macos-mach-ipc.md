# macOS Mach IPC

The Mach kernel surface above the [BSD syscall layer](macos-kernel-core.md): the port namespace,
`mach_msg2`, the MIG subsystems sogen answers, the IOKit/IOSurface user client, and XPC bootstrap. This
is the layer that turns a process able to make syscalls into one that can talk to daemons, receive its
own event port, and drive a window. It cross-links [concurrency](macos-concurrency.md) and
[memory](macos-memory.md) — a workqueue worker's identity, a psynch wait and a shared page all live at
this boundary — and is consumed by [`macos-window-server.md`](macos-window-server.md).

## Status

Working: the port namespace with generation-tagged names and port sets, `mach_msg2`'s vector form
including auxiliary data, the MIG subsystems named below, the IOKit registry and the IOSurface user
client, and an XPC bootstrap responder that answers or honestly refuses every service by name. Sogen
runs no daemons; a lookup either gets a live connection whose messages are refused one at a time, or —
for the two names measured to need it — a failed lookup.

## The port namespace and port sets

A Mach port name is `(index << 8) | generation`, and generation reuse is observable guest-visible
behaviour, not an implementation detail: index 8 was measured reused within a single process run, first
as a host port and then, after `mach_port_deallocate`, as the bootstrap port. A port-identification
scheme that keys on
index alone will silently answer the wrong request.

**Port sets are load-bearing for CFRunLoop, not a convenience.** CFRunLoop's main wait port is a set, not
a single port, and every run loop hung without set support — a receive named on a set drains whichever
member has a message, and a knote must fire through set membership as well as through the individual
port. `mach_port_namespace.cpp` and `mach_traps.cpp` implement the
insert/extract/first-queued-member/sets-containing traps (20/22/23) this depends on.

## `mach_msg2`'s vector form

`MACH64_MSG_VECTOR` (`0x1_0000_0000`) replaces the trap's flat message buffer with an array of exactly
two `mach_msg_vector_t` elements — index 0 the message, index 1 auxiliary data — never more. Both size
halves of the trap carry an **element count**, not a byte count, and the count is present in the send
half even for a receive-only call. sogen's
`mq_call && send_size < MSG_HEADER_SIZE` heuristic used to select vector calls *by accident* — the
element count 2 happens to be smaller than a 24-byte header — and the same accident made `deliver_reply`
treat the receive element count as a byte budget, so every real reply on this path came back
`MACH_RCV_TOO_LARGE`.

`decode_msg2_call` (`src/macos-emulator/mach/mach_msg.cpp`) resolves the vector form before anything
downstream sees the call, so nothing else in the stack knows the form exists. The real byte sizes live
in `msgv[0].msgv_send_size` / `msgv[0].msgv_rcv_size`; `msgh_size` in the buffer is frequently zero
because the kernel fills it in, and is not authoritative. Auxiliary data is not currently forwarded —
sogen's message queue holds a byte vector per message with nowhere to put a second buffer — and every
reply sogen synthesises carries a zeroed 8-byte aux header, which is exactly the answer the real kernel
gives a receiver that asked for aux on a message that had none.

`__CFRunLoopServiceMachPort` — the wait every AppKit process lives in — is receive-only, vector, with a
`0xc00`-byte message budget and `0x80`-byte aux budget; getting this path right is what makes an idle
run loop distinguishable from a hang at all (see [`macos-browser.md`](macos-browser.md) for the
idle-vs-deadlock distinction this feeds).

## The MIG subsystems sogen answers, with their bases

| Base | Subsystem | What sogen answers |
| --- | --- | --- |
| 200 | `mach_host` | `host_info` (200), `host_get_clock_service` (206) |
| 400 | `host_priv` | `host_get_special_port` (412) |
| 3200 | `mach_port` | `mach_port_extract_right` (3215), `mach_port_set_attributes` (3218) — the other 41 of 43 routines are unimplemented |
| 3400 | `task` | `task_info` (3405), `task_get_special_port` (3409), `task_set_special_port` (3410), `task_set_exception_ports` (3413), `task_get_exception_ports` (3414), `task_swap_exception_ports` (3415), `semaphore_create` (3418), `task_name_for_pid` (mach trap 44), `task_create_identity_token` (3457) |
| 4800 | `mach_vm` | `_kernelrpc_mach_vm_map` (4811), `mach_vm_remap` (4813), `mach_vm_copy` (4807), `mach_make_memory_entry_64` (4817), `mach_vm_deferred_reclamation_buffer_allocate` (4822, stubbable) |
| 8000 | `task_restartable` | 8000/8001, undocumented in Apple's shipped SDK and recovered by wire capture |
| 29000/30000/32000/33000/34000/40200 | SkyLight / QuartzCore (`CGXRendezvous`/`CGXServices`/`CGXWindowServer`/`WSXServicesRelocated`/`WSXServerRelocated`/`CASCARenderServices`) | the window-server surface — see [`macos-window-server.md`](macos-window-server.md) |
| 2800–2889 | IOKit (`io_object_*`, `io_registry_entry_*`, `io_service_*`) | the registry walk and property reads §3 below names |

Dispatch is keyed on **`(destination port kind, msgh_id)`, never on `msgh_id` alone** — notifyd's ids
`1012`/`1023` sit numerically inside the range Apple's own `clock.defs` claims for `mach_host`, and are
distinguished only by which port they arrive on.

`mach_port` (3200), the 43-routine port-operations MIG subsystem, contributed **zero** routines to the
bring-up trace measured for this section — every port operation it observed went through the
dedicated `_kernelrpc_mach_port_*` traps instead. That was an observation about one trace, not a claim
about the whole subsystem: sogen has since
implemented two of its 43 routines for a fuller GUI run, `mach_port_extract_right` (3215) and
`mach_port_set_attributes` (3218) (`src/macos-emulator/mach/mig_routines_task.cpp`); the other 41 remain
unimplemented.

## The IOKit user client

IOKit's client stubs call `mach_msg2_internal` directly rather than the `mach_msg` a
`DYLD_INSERT_LIBRARIES` interposer can see, so every routine id below was read out of the immediate
loaded into `x4` at each stub's disassembly. `io_server_version` (2877) is sent first and
selects one of two families: answered, the client uses the binary-OSSerialize `_bin` routines (2880,
2881, 2888, 2889); refused, it falls back to an XML-encoded legacy family (2873, 2804, 2805, 2811). Answering 2877 is what moves a guest onto
the fully-measured `_bin` family.

**`IOSurfaceLock`/`Unlock` are `iokit_user_client_trap` — mach trap 100 — and therefore never appear in
any MIG trace at all.** `IOConnectTrapN` (indices 0–5: increment/decrement use count, lock, unlock,
release, retain) bypasses MIG entirely; a MIG trace of a locking client shows nothing between the
surface's creation and its release. `src/macos-emulator/mach/mach_traps.cpp`
registers trap 100 as `iokit_user_client_trap`; `io_surface_user_client.{hpp,cpp}` implements selectors
0, 9, 10, 13, 20, 35 of `io_connect_method` (2865) and trap indices 0–5, backing the 3176-byte surface
record the client copies to its own `+0x70` and reads with fixed-offset loads thereafter.

## The two binary-OSSerialize rules that only matter when reading

sogen's own OSSerialize encoder never needed these, because it only ever wrote. Decoding a dictionary a
**guest** wrote — as the IOKit registry does for a `io_connect_method` create call — needs two more
rules:

- **A number (`kOSSerializeNumber`, type `0x04`) always carries 8 payload bytes**, regardless of what its
  length field says — the length field is the declared bit width (`0x20` or `0x40`), not a byte count.
- **An object (`kOSSerializeObject`, type `0x0c`) is a zero-based back-reference** into *every* object
  unserialised so far, in parse order, with the enclosing collection registered before its children and
  keys counted the same as values. CoreUI's own create dictionary uses one — its `IOSurfaceHeight` entry
  is a back-reference to the already-parsed `IOSurfaceWidth` number. Resolving the index against values
  only, or as one-based, reads a key symbol as the height and produces a surface with no pixels.

## XPC check-in and `decide_xpc_service`

Sogen runs no daemons, so it cannot hand a lookup a real one — but it can still hand back a **live
connection whose every message is refused**, which is what most clients cope with best: CoreFoundation
falls back to reading preferences from disk, TCC reads as denied, and libxpc's own error-recovery path
never engages. A **failed** lookup is the harsher answer: libxpc leaves the connection with no send
right, and a synchronous send on it waits forever for a reply nothing will generate.

`decide_xpc_service` (`src/macos-emulator/mach/xpc_services.cpp`) is therefore name-independent and
exception-based: every service name gets a live, refused port **except** two, measured to be exactly the
ones whose clients treat only a failed lookup as terminal —

- `com.apple.logd` — `libsystem_trace`'s firehose push dies with `MIG_REPLY_MISMATCH` against a
  live-but-dead logd connection, but runs its own no-logd degradation when the lookup itself fails.
- `com.apple.runningboard` — `-[RBSConnection _handshake]` retries a refused handshake exactly 1000
  times and then throws `NSInternalInconsistencyException`, killing the process; a failed lookup instead
  lets the app carry on into its run loop.

`com.apple.windowserver.active` is the one name sogen answers **for real**, because sogen is the window
server: the lookup hands back the window-server root port that `gui/macos_window_server_mig.cpp` serves.
Failing this particular lookup makes `CGSLookupServerPort` return NULL and HIToolbox's
`_CheckEventsInited` bail before the Apple Event that AppKit posts
`NSApplicationDidFinishLaunchingNotification` from is ever synthesised — so no app ever built a
window.

A message a refused connection receives is answered with an empty dictionary — tccd's own measured
refusal shape — because no dictionary a daemon sends can arrive at the client as an `XPC_TYPE_ERROR`
object; error objects are fabricated entirely client-side by libxpc. NSXPC invocations get the same
treatment one layer up: a reply whose signature is the request's own `replysig` with the byte offsets
stripped and every argument nil is enough for LaunchServices to log a failure and fall back to its own
local database rather than hang — validated end to end on the host with an interposer that answers every
XPC connection this way and still lets a real AppKit window appear.

## Still open

- **`com.apple.lsd.mapdb`.** The empty-dictionary-per-message refusal above works for tccd and for
  cfprefsd; LaunchServices instead retries it hundreds of times with no window ever appearing, and a
  failed lookup parks libxpc forever in a synchronous send. Neither refusal shape works for this one
  daemon; it needs a real answer (see [`macos-emulation.md`](macos-emulation.md), "What does not work").
- **`0x10000000` asynchronous XPC sends are left unanswered.** The reply for a synchronous send
  (`0x40000000`) is delivered correctly; an asynchronous send's answer belongs on a per-call send-once
  port that libdispatch's kqueue registers with `MACH_RCV_MSG` in `fflags`, so the kernel is expected to
  deliver the message *inside* the kevent itself. sogen's kqueue only signals that a message is
  available, which is not the same contract, and routing the reply the two ways tried both crash the
  guest differently. The real fix
  belongs in `mach_msg.cpp` plus `EVFILT_MACHPORT`.
- **Planar IOSurfaces are not modelled**, and a multi-plane create reports itself by name rather than
  guessing at a layout (`src/macos-emulator/mach/io_surface_user_client.cpp`).
- **The role of 32006's second memory entry and the u32 at `+0x44` of the 40202 reply remain unknown** —
  measured to exist, not measured to matter.
- **Smaller named gaps a real app has already hit, none yet a wall**: `io_*` routines beyond the ones
  §3 names, MIG 10050 on coreservicesd, MIG 1011/1016/1026 on service ports, XPC routine `0x4000012d` on
  the bootstrap port, `shm_open("apple.shm.notification_center")`, and an all-zero sysctl MIB. Each
  reports itself by name rather than failing silently.
