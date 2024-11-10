<h1 align="center">
	Windows User Space Emulator
	<br>
	<a href="https://github.com/momo5502/emulator?tab=GPL-2.0-1-ov-file"><img src="https://img.shields.io/github/license/momo5502/emulator?color=00B0F8"/></a>
	<a href="https://github.com/momo5502/emulator/actions"><img src="https://img.shields.io/github/actions/workflow/status/momo5502/emulator/build.yml?branch=main&label=build"/></a>
	<a href="https://github.com/momo5502/emulator/issues"><img src="https://img.shields.io/github/issues/momo5502/emulator?color=F8B000"/></a>
	<img src="https://img.shields.io/github/commit-activity/m/momo5502/emulator?color=FF3131"/>
</h1>

A high-performance Windows process emulator that operates at the syscall level, providing full control over process execution through comprehensive hooking capabilities.

Built in C++ and powered by the Unicorn Engine.

##
> [!NOTE]  
> The project is still in a very early, prototypy state. The code still needs a lot of cleanup and many features and syscalls need to be implemented. However, constant progress is being made :)

## Key Features

* __Syscall-Level Emulation__: Instead of reimplementing Windows APIs, the emulator operates at the syscall level, allowing it to leverage existing system DLLs
* __Advanced Memory Management__: Supports Windows-specific memory types including reserved, committed, built on top of Unicorn's memory management
* __Complete PE Loading__: Handles executable and DLL loading with proper memory mapping, relocations, and TLS
* __Exception Handling__: Implements Windows structured exception handling (SEH) with proper exception dispatcher and unwinding support
* __Threading Support__: Provides a scheduled (round-robin) threading model
* __State Management__: Supports both full state serialization and fast in-memory snapshots
* __Debugging Interface__: Implements GDB serial protocol for integration with common debugging tools (IDA Pro, GDB, LLDB, VS Code, ...)

Perfect for security research, malware analysis, and DRM research where fine-grained control over process execution is required.

## Building

Make sure to clone the repo including all submodules.

```bash
git clone https://github.com/momo5502/emulator.git
cd emulator
git submodule update --init --recursive
```

At the moment, the project is only compatible with 64 bit Windows, but that is being worked on: <a href="https://github.com/momo5502/emulator/issues/17">Issue 17</a>

It requires CMake and uses CMake presets. Make sure to open an x64 Dev Cmd before running any of the commands.

### Visual Studio 2022

To generate a Visual Studio solution, execute the following command:

```bash
cmake --preset=vs2022
```

The solution will be at `build/vs2022/emulator.sln`.

### Ninja

To build the debug version using Ninja run:

```bash
cmake --workflow --preset=debug
```

You can also build the release variant:

```bash
cmake --workflow --preset=release
```


### Running Tests

CTest is used for testing.

In Visual Studio, build the `RUN_TESTS` target.

With Ninja, execute the CTest command in the ninja build folder (e.g. `build/release/`):

```bash
ctest
```

## Preview

![Preview](./docs/images/preview.jpg)

## YouTube Overview

[![YouTube video](./docs/images/yt.jpg)](https://www.youtube.com/watch?v=wY9Q0DhodOQ)

Click <a href="https://docs.google.com/presentation/d/1pha4tFfDMpVzJ_ehJJ21SA_HAWkufQBVYQvh1IFhVls/edit">here</a> for the slides.
