# Documentation index

Sogen's design and status notes, one file per subsystem. Every row below is one document; each covers
a piece of the emulator that is not obvious from reading its source alone.

## Backends

| Document | Covers |
| --- | --- |
| [`arm64-backend.md`](arm64-backend.md) | The AArch64 guest backend on Unicorn — registers, memory, hooks, and the only CPU engine available under wasm. |
| [`fex-backend.md`](fex-backend.md) | The FEX-Emu backend, which JITs x86/x86-64 guest code to ARM64 so Windows binaries run fast on Apple Silicon hosts. |
| [`hvf-backend.md`](hvf-backend.md) | Hypervisor.framework — runs arm64 guest code natively on Apple Silicon instead of interpreting it. |
| [`kvm-backend.md`](kvm-backend.md) | The Linux KVM backend, mirroring the WHP backend's structure for x86_64 Linux hosts. |
| [`unicorn-aarch64-patch.md`](unicorn-aarch64-patch.md) | Four upstream Unicorn bugs found and patched while bringing up the `aarch64-softmmu` target. |

## Windows

| Document | Covers |
| --- | --- |
| [`windows-ui-emulation.md`](windows-ui-emulation.md) | Status of moving Windows USER/GDI pixel and input ownership into guest code. |
| [`gpu-paravirtualization.md`](gpu-paravirtualization.md) | Vulkan API remoting — a guest's Vulkan calls run against the host's real driver instead of an emulated GPU. |
| [`steam-bridge.md`](steam-bridge.md) | Forwards a guest game's Steamworks calls to the host's real Steam client instead of reimplementing Steamworks. |
| [`steam-bridge-versioning.md`](steam-bridge-versioning.md) | A prototype libclang-based generator for exact Steamworks interface-version shims; validated, not yet integrated. |

## Linux

No Linux-specific narrative document exists yet. The Linux-hosted execution engine is
[`kvm-backend.md`](kvm-backend.md), filed under Backends above because its design mirrors the other
CPU backends rather than a guest-OS personality.

## macOS

| Document | Covers |
| --- | --- |
| [`macos-emulation.md`](macos-emulation.md) | Entry point: what runs today, the layer-by-layer architecture, how to run it, and what does not work. |
| [`macho-loader.md`](macho-loader.md) | Stage 2 — mapping the guest's executable and dyld into an AArch64 address space; sogen maps, dyld links. |
| [`macos-kernel-core.md`](macos-kernel-core.md) | Stage 3 — the BSD syscall layer between the Mach-O loader and the arm64 backend. |
| [`macos-mach-ipc.md`](macos-mach-ipc.md) | The Mach port namespace, `mach_msg2`'s vector form, the MIG subsystems sogen answers, IOKit/IOSurface, XPC. |
| [`macos-concurrency.md`](macos-concurrency.md) | The cooperative scheduler, the workqueue pool, psynch mutexes/condvars, `ulock`, `EVFILT_TIMER`. |
| [`macos-memory.md`](macos-memory.md) | The guest/emulator address-space split and the GUI arena's lifetime rule. |
| [`macos-window-server.md`](macos-window-server.md) | SkyLight connection bring-up, the CALayer tree, the software compositor, SDF bezels, and four killed theories. |
| [`macos-browser.md`](macos-browser.md) | Running sogen in a browser tab: the wasm64 build, the synchronous range bridge, idle-vs-deadlock, the root cache. |
| [`macos-tracing.md`](macos-tracing.md) | `analyzer --os=macos` syscall tracing: decoded BSD syscall arguments and errno on every line. |

## Shared

| Document | Covers |
| --- | --- |
| [`multi-vcpu-design.md`](multi-vcpu-design.md) | In-progress design for running more than one vCPU per emulator instance. |

## What belongs here

A document in this directory is the narrative: the ABI facts that forced a design, the behaviours that
shaped it, and the gaps the next stage inherits. It is not a restatement of the source — if a reader can
learn something by reading the code, it does not belong here.

The numbers are measured rather than estimated, and a document says what it measured them against. Where
a document and the emulator disagree, the emulator is the ground truth: file the correction against the
document.
