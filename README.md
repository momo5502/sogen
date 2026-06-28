<h1 align="center">
	<a href="https://github.com/momo5502/sogen"><img src="https://momo5502.com/sogen/banner.png" height="300" /></a>
	<br>
	<a href="https://github.com/momo5502/sogen?tab=GPL-2.0-1-ov-file"><img src="https://img.shields.io/github/license/momo5502/sogen?color=00B0F8"/></a>
	<a href="https://github.com/momo5502/sogen/actions"><img src="https://img.shields.io/github/actions/workflow/status/momo5502/sogen/build.yml?branch=main&label=build"/></a>
	<a href="https://github.com/momo5502/sogen/issues"><img src="https://img.shields.io/github/issues/momo5502/sogen?color=F8B000"/></a>
	<img src="https://img.shields.io/github/commit-activity/m/momo5502/sogen?color=FF3131"/>
</h1>

Sogen runs Windows and Linux programs without a real operating system, and lets you see and control everything they do.

Instead of reimplementing thousands of OS APIs, Sogen emulates binaries at the CPU and syscall level and runs the **real system DLLs**, so behavior closely matches the real OS.

Every instruction, memory access and API call can be hooked, inspected or rewritten, runs are fully deterministic, and the entire emulator state can be snapshotted and restored.

Built in C++ and powered by the CPU backend of your choice:

- [Unicorn Engine](https://github.com/unicorn-engine/unicorn)
- [icicle-emu](https://github.com/icicle-emu/icicle-emu)
- [Hyper-V (WHP)](https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform)
- [KVM](https://www.kernel.org/doc/html/latest/virt/kvm/api.html)

Try it out: <a href="https://sogen.dev">sogen.dev</a>

## Key Features

- **Real system DLLs**: runs the actual ntdll, kernel32 and user32, not reimplemented stubs
- **Hook & rewrite**: intercept and change memory, instructions, syscalls and API calls
- **Faithful Windows internals**: PE loading (relocations, TLS), Windows memory types, SEH, threading, the registry, filesystem and networking
- **Snapshot & restore**: full state serialization, fast in-memory snapshots and minidump loading
- **Runs everywhere**: Windows, Linux, macOS, Android, iOS and the browser, on x86-64 and arm64
- **Deterministic**: every run is reproducible, down to the instruction

## Preview

<img src="https://momo5502.com/sogen/preview.svg" width="650" alt="Preview" />

## Undetectable Debugging

Debug with the tools you already know, like IDA Pro or GDB, over the GDB protocol, or use the built-in in-browser debugger.  
The debugger runs at the emulator level, outside the process, so it stays invisible to anti-debug checks.

<img src="https://momo5502.com/sogen/debugger.png" width="650" alt="Debugging a process running in Sogen from an IDA Pro remote GDB session" />
&nbsp;  

## Run Games in a Sandbox

Native GUI apps run, with working windows, dialogs and controls.  
GPU paravirtualization enables 3D acceleration on your real GPU, while the Hyper-V backend runs the code natively on your CPU. Fast enough for games.  
DirectX titles run through [DXVK](https://github.com/doitsujin/dxvk), which translates Direct3D to Vulkan on top of the GPU bridge.

<img src="https://momo5502.com/sogen/game.png" width="650" alt="A game running inside the Sogen emulator" />
&nbsp;  

## Project Overview

<a href="https://www.youtube.com/watch?v=wY9Q0DhodOQ" target="_blank">
  <img src="https://momo5502.com/sogen/video.png" alt="YouTube Video" width="600" />
</a>

Click <a href="https://docs.google.com/presentation/d/1pha4tFfDMpVzJ_ehJJ21SA_HAWkufQBVYQvh1IFhVls/edit">here</a> for the slides.
&nbsp;  

## Python Bindings

Install with:

```bash
pip install sogen
```

Python bindings require an emulation root. You can download a ready-made root [here](https://sogen.dev/root.zip), or create your own by following the instructions in the [wiki](https://github.com/momo5502/sogen/wiki/Run-The-Emulator#emulation-root-environment).

Example:

```python
import sogen

emu = sogen.windows.create_application("c:/test-sample.exe", emulation_root="./root")


def on_module_load(module):
    if module.name.lower() == "test-sample.exe":
        emu.hooks.memory_execution_at(module.entry_point, lambda address: print(f"hit entry point: 0x{address:x}"))

emu.callbacks.on_module_load = on_module_load
emu.start()
print(emu.process.exit_status)
```

See `examples/python/README.md` for setup details and a larger example.

## Quick Start (Windows + Visual Studio)

> [!TIP]  
> Checkout the [Wiki](https://github.com/momo5502/sogen/wiki) for more details on how to build & run the emulator on Windows, Linux, macOS, ...

1\. Checkout the code:

```bash
git clone --recurse-submodules https://github.com/momo5502/sogen.git
```

2\. Run the following command in an x64 Development Command Prompt in the cloned directory:

```bash
cmake --preset=vs2022
```

3\. Build the solution that was generated at `build/vs2022/emulator.sln`

4\. Create a registry dump by running the [grab-registry.bat](https://github.com/momo5502/sogen/blob/main/src/tools/grab-registry.bat) as administrator and place it in the artifacts folder next to the `analyzer.exe`

5\. Run the program of your choice:

```bash
analyzer.exe C:\example.exe
```
