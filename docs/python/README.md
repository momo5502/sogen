<p align="center">
  <a href="https://github.com/momo5502/sogen"><img src="https://momo5502.com/sogen/banner.png" height="220" alt="Sogen" /></a>
</p>

Sogen exposes Python bindings for Windows and Linux userspace emulation. The Python API is meant for scripting runs, building small analysis helpers, and quickly iterating on callbacks; Windows bindings also expose hooks for deeper analysis.

Install from PyPI:

```bash
pip install sogen
```

Project links:

- PyPI: https://pypi.org/project/sogen/
- Repository: https://github.com/momo5502/sogen
- Ready-made emulation root: https://sogen.dev/root.zip

## What you need

The Python package still needs an **emulation root** at runtime. Download ready-made root here:

- https://sogen.dev/root.zip

Extract it somewhere convenient, for example:

```text
./root
```

Most examples in this document use:

```python
emulation_root="./root"
```

## Quick start

```python
import sogen

app = sogen.windows.create_application(
    "c:/test-sample.exe",
    emulation_root="./root",
)

app.callbacks.on_stdout = lambda text: print(text, end="")
app.start()
print("exit status:", app.process.exit_status)
```

Linux quick start:

```python
import sogen

syscalls = []


def on_syscall(syscall_id, syscall_name):
    syscalls.append((syscall_id, syscall_name))
    return sogen.HookContinuation.run


app = sogen.linux.create_application(
    "/bin/true",
    emulation_root="/",
    backend=sogen.Backend.unicorn,
)

app.callbacks.on_syscall = on_syscall
app.start()
print("exit status:", app.process.exit_status)
print("syscalls observed:", len(syscalls))
```

`sogen.linux` exposes its own Linux-scoped factories, including `sogen.linux.create_application(...)` and `sogen.linux.create_empty(...)`; it does not use the root-level compatibility aliases.

## Minimal example with file + port mappings

Windows example:

```python
from pathlib import Path
import sogen

app = sogen.windows.create_application(
    "c:/test-sample.exe",
    emulation_root="./root",
    path_mappings={"c:/a.txt": Path("./a.txt")},
    port_mappings={28970: 28980},
)

app.callbacks.on_stdout = lambda text: print(text, end="")
app.start()
print("exit status:", app.process.exit_status)
```

Linux applications also accept guest cwd, filesystem mapping, and TCP port mapping kwargs:

```python
from pathlib import Path
import sogen

app = sogen.linux.create_application(
    "/bin/my-tool",
    emulation_root="./linux-root",
    working_directory="/work",
    path_mappings={"/work/input": Path("./fixtures/input")},
    read_only_path_mappings=[("/work/readonly", Path("./fixtures/readonly"))],
    port_mappings={8080: 18080},
)

assert app.get_host_port(8080) == 18080
assert app.get_emulator_port(18080) == 8080
```

Linux `working_directory` is normalized as an absolute guest path and is active
before application load/startup, so it applies to interpreter/loader work as well
as `getcwd()` and cwd-relative syscalls such as `open()` and
`openat(AT_FDCWD, ...)`. `path_mappings` and `read_only_path_mappings` are
also installed before load/startup, making mapped guest paths visible to
interpreter startup and later filesystem operations. They accept either
`{guest_path: host_path}` dictionaries or `[(guest_path, host_path), ...]`
sequences, and the longest matching guest prefix wins. Read-only mappings reject
mutating file operations including write-open, write, truncate, chmod, unlink,
and rename. `port_mappings` accepts `{emulator_port: host_port}` or
`[(emulator_port, host_port), ...]`; mapped IPv4 loopback TCP `connect()` calls
are proxied to the configured host loopback port, while unmapped ports keep the
synthetic Linux socket behavior.

Path and port mappings explicitly opt into host filesystem and network access. They are convenience bridges for tests and controlled analysis workflows, not a malware sandbox boundary.

## Choosing backend

`sogen.windows.create_empty()`, `sogen.windows.create_application()`, `sogen.linux.create_empty()`, and `sogen.linux.create_application()` accept `backend=sogen.Backend.unicorn`.

```python
import sogen

emu = sogen.windows.create_empty(
    emulation_root="./root",
    backend=sogen.Backend.unicorn,
)
```

Available values:

- `sogen.Backend.unicorn`
- `sogen.Backend.icicle`
- `sogen.Backend.whp` (Windows)
- `sogen.Backend.kvm` (Linux x86_64)

Default is `sogen.Backend.unicorn`.

## High-level structure

Windows entry points:

- `sogen.windows.create_empty(...)`
- `sogen.windows.create_application(...)`

Linux entry points:

- `sogen.linux.create_empty(...)`
- `sogen.linux.create_application(...)`

Windows compatibility aliases currently remain at top level and are Windows-specific:

- `sogen.create_empty(...)` -> `sogen.windows.create_empty(...)`
- `sogen.create_application(...)` -> `sogen.windows.create_application(...)`

Objects exposed by the Windows bindings:

- `sogen.windows.Emulator` / `sogen.windows.WindowsEmulator`
- `ProcessContext`
- `Thread`
- `MemoryManager`
- `Hooks`
- `Callbacks`

Linux-specific objects exposed by the bindings:

- `sogen.linux.Emulator` / `sogen.linux.LinuxEmulator`
- `sogen.linux.ProcessContext`
- `sogen.linux.MemoryManager`
- `sogen.linux.Thread`
- `sogen.linux.MemoryRegionInfo`
- `sogen.linux.Hook`
- `sogen.linux.Hooks`
- `sogen.linux.Callbacks`
- `sogen.linux.ExportedSymbol`
- `sogen.linux.MappedSection`
- `sogen.linux.LinuxMappedModule`
- `sogen.linux.SymbolHooks`
- `sogen.linux.LinuxSymbolCall`
- `sogen.linux.ThreadWaitState`

Linux callback slots:

- `app.callbacks.on_stdout`
- `app.callbacks.on_stderr`
- `app.callbacks.on_syscall`
- `app.callbacks.on_memory_violate`
- `app.callbacks.on_memory_allocate`
- `app.callbacks.on_memory_protect`
- `app.callbacks.on_memory_release`
- `app.callbacks.on_module_load`
- `app.callbacks.on_thread_create`
- `app.callbacks.on_thread_terminated`
- `app.callbacks.on_thread_switch`

Common Windows workflows:

- run application with `app.start()`
- watch output with `app.callbacks.on_stdout`
- react to module loads with `app.callbacks.on_module_load`
- intercept WinAPI calls with `app.hooks.apis[...]`
- read/write emulator memory with `read_memory()` / `write_memory()`
- save and restore state with `save_snapshot()` / `restore_snapshot()`

## Callbacks

Example: print loaded modules.

```python
import sogen

app = sogen.windows.create_application(
    "c:/test-sample.exe",
    emulation_root="./root",
)


def on_module_load(module):
    print(f"loaded {module.name} @ 0x{module.entry_point:x}")


app.callbacks.on_module_load = on_module_load
app.start()
```

Useful Linux callback slots:

- `app.callbacks.on_stdout`
- `app.callbacks.on_stderr`
- `app.callbacks.on_syscall`
- `app.callbacks.on_memory_violate`
- `app.callbacks.on_memory_allocate`
- `app.callbacks.on_memory_protect`
- `app.callbacks.on_memory_release`
- `app.callbacks.on_module_load`
- `app.callbacks.on_thread_create`
- `app.callbacks.on_thread_terminated`
- `app.callbacks.on_thread_switch`

Useful Windows callback slots:

- `app.callbacks.on_stdout`
- `app.callbacks.on_syscall`
- `app.callbacks.on_memory_violate`
- `app.callbacks.on_module_load`
- `app.callbacks.on_module_unload`

Linux `on_syscall` callbacks use `callback(syscall_id: int, syscall_name: str) -> sogen.HookContinuation | bool | None`.
`None`, `False`, and `sogen.HookContinuation.run` continue the built-in syscall handler.
`True` and `sogen.HookContinuation.skip` suppress the built-in handler, so an intercepting callback must update guest registers and memory itself.

Linux `on_module_load` callbacks use `callback(module: sogen.linux.LinuxMappedModule) -> None`.
Assigning `app.callbacks.on_module_load` replays modules already mapped during
application construction in ascending image-base order, then observes future
ELF modules discovered from runtime file-backed `mmap()` activity (for example
libc mapped by the Linux dynamic loader). Linux currently exposes no module-unload
callback because there is no native Linux unload event source in the emulator.

Linux thread controls expose the active emulated thread and cooperative native
scheduler hooks:

- `app.current_thread` returns the active `sogen.linux.Thread` or `None`.
- `app.current_thread_id` returns the active TID or `None`.
- `app.activate_thread(tid)` switches to a live thread, saving/restoring CPU
  registers, and returns `False` for missing or terminated threads.
- `app.perform_thread_switch()` chooses the next runnable non-terminated thread
  in TID order and returns whether a switch happened.
- `app.yield_thread()` performs that switch and stops the backend if it switched.
- `app.process.threads` lists non-terminated Linux threads; each thread exposes
  `tid`, `current_ip`, `start_address`, `wait_state`, `setup_done`, stack/TLS
  fields, termination state, exit code, and executed-instruction count.
  `previous_ip` raises `NotImplementedError` until Linux tracks a prior
  instruction pointer instead of silently returning a placeholder value.

Linux thread callbacks use native lifecycle/switch paths:

- `app.callbacks.on_thread_create(thread)` fires after `clone(CLONE_THREAD)`
  creates a thread.
- `app.callbacks.on_thread_terminated(thread)` fires when a Linux thread exits.
- `app.callbacks.on_thread_switch(old_tid, new_tid)` fires when the active TID
  changes through `activate_thread()` or the scheduler.



Linux emulators expose stop diagnostics after every `start()` call:

- `app.last_stop_reason` is a string: `"none"`, `"unknown_syscall"`,
  `"unimplemented_syscall"`, `"syscall_exception"`, `"instruction_limit"`,
  `"normal_exit"`, `"signal_termination"`, `"unhandled_memory_violation"`,
  `"explicit_stop"`, `"backend_error"`, `"breakpoint"`, or `"watchpoint"`.
- `app.last_stop_reason_code` is the integer enum value for integrations that
  prefer stable numeric storage.
- `app.last_stop_detail` contains reason-specific detail, for example
  `address=0x... size=...` for an unhandled memory violation or `count=...`
  for an instruction limit.

Linux signal callbacks use `app.callbacks.on_signal(signum, fault_addr, si_code)`.
The callback fires at the start of native signal delivery, before the emulated
process's handler or Linux's default termination path runs. It is observational:
return values are ignored and cannot suppress guest delivery. `on_exception` is a
Linux alias for the exact same callback slot and receives the same `(signum,
fault_addr, si_code)` payload because Linux exceptions surface as signals. This
alias is also shown by `repr(app.callbacks)` for runtime discoverability.

Linux memory violation callbacks use
`callback(address: int, size: int, operation: sogen.MemoryOperation, type: sogen.MemoryViolationType) -> sogen.MemoryViolationContinuation | bool | None`.
`sogen.MemoryOperation` is an alias of `sogen.MemoryPermission`, matching the
backend operation values. `None`, `True`, and
`sogen.MemoryViolationContinuation.resume` resume execution; `False` and
`sogen.MemoryViolationContinuation.stop` stop with
`last_stop_reason == "unhandled_memory_violation"`; and
`sogen.MemoryViolationContinuation.restart` asks the backend to retry the
faulting instruction. If no Linux memory violation callback or hook is
installed, the emulator keeps its native SIGSEGV/default-stop path.

## Linux low-level hooks

Linux emulators expose low-level hooks through `app.hooks`. Most methods are
backend hooks; `memory_violation(callback)` is a Linux-managed observer that
coexists with the emulator's internal SIGSEGV delivery path. Hook methods return
`sogen.linux.Hook` handles with a read-only `active` property and an idempotent
`remove()` method. Returned handles also stay active if you do not store them:
`app.hooks` owns the hook until it is removed or the emulator is destroyed.

```python
import sogen

emu = sogen.linux.create_empty(emulation_root="/")
emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
emu.write_memory(0x100000, b"\x0f\xa2")  # cpuid
emu.write_register(sogen.Register.rip, 0x100000)


def on_cpuid(data):
    emu.stop()
    return sogen.HookContinuation.skip


hook = emu.hooks.instruction(sogen.Instruction.cpuid, on_cpuid)
emu.start(10)
hook.remove()
```

Available Linux hook methods are:

- `app.hooks.memory_execution(callback)`
- `app.hooks.memory_execution_at(address, callback)`
- `app.hooks.memory_read(address, size, callback)`
- `app.hooks.memory_write(address, size, callback)`
- `app.hooks.instruction(sogen.Instruction.<name>, callback)`
- `app.hooks.interrupt(callback)`
- `app.hooks.memory_violation(callback)`
- `app.hooks.basic_block(callback)`

Linux `Hooks` exposes Linux symbol hooks as `app.hooks.symbols`. Windows-only
`hooks.apis` remains absent under `sogen.linux`.

## Linux modules and symbol hooks

Linux applications expose initial ELF modules before `app.start()` and append
runtime-loaded ELF modules as the dynamic loader maps them:

```python
import ctypes
import sogen

app = sogen.linux.create_application("/bin/true", emulation_root="/")
for module in app.modules:
    print(module.name, hex(module.image_base), module.path)
```

Each `sogen.linux.LinuxMappedModule` has `name`, `path`, `image_base`,
`size_of_image`, `entry_point`, `exports`, `needed_libraries`, `sections`,
`rpath`, and `runpath`. Exports are `sogen.linux.ExportedSymbol` objects with
`name`, `rva`, and `address`; sections are `sogen.linux.MappedSection` objects
with `name`, `start`, `length`, and `permissions`. Executable ELF sections and
regions expose composite permissions such as `sogen.MemoryPermission.read_exec`;
write-execute mappings use `sogen.MemoryPermission.write_exec`. Use
`app.find_module_by_address(address)` or `app.find_module_by_name(name)` to look
up one module.

Symbol hooks are registered with `app.hooks.symbols["symbol"] = callback` or
`app.hooks.symbols["module!symbol"] = callback`. The qualified form matches the
Linux module `name` or filename stem case-sensitively. Assign `None` to a key to
delete that hook, or call `app.hooks.symbols.clear()` to remove all symbol hooks.

Use `@sogen.linux.symbol_call(params=[...], restype=...)` to describe an
x86_64 System V integer/pointer signature. Supported ctypes are integer and
pointer-sized scalar/pointer ctypes up to 8 bytes; unsupported ctypes fail when
the hook is registered. Parameters are decoded from `rdi`, `rsi`, `rdx`, `rcx`,
`r8`, `r9`, then stack slots after the return address.

```python
@sogen.linux.symbol_call(params=[ctypes.c_int], restype=ctypes.c_int)
def on_target(call, params):
    print(call.module.name, call.name, params[0])
    return sogen.ApiContinuation.run_original


app.hooks.symbols["target_function"] = on_target
```

Callbacks receive `sogen.linux.LinuxSymbolCall` with read-only `module`, `name`,
`address`, and `return_address`, plus read-write `return_value`. Returning
`None`, `False`, or `sogen.ApiContinuation.run_original` continues the original
symbol. Returning `True` or `sogen.ApiContinuation.intercept` skips the original
symbol by writing `return_value` to `rax`, setting `rip` to the saved return
address, and advancing `rsp` by 8.

Backend support differs by engine. Unicorn and Icicle support fine-grained
execution, read/write, instruction, interrupt, basic-block, and symbol hooks.
KVM registers these hooks for API compatibility, but read/write, execution,
basic-block, and symbol callbacks may not fire on that backend.

## Linux debugger facade

Linux emulators expose `app.debug`, a Python-native debugger facade over the
same backend hooks and CPU state used by low-level Linux hooks. Breakpoints are
execute hooks: `app.debug.set_breakpoint(address)` arms one,
`clear_breakpoint(address)` removes it, and `list_breakpoints()` returns the
currently armed addresses. When execution reaches a breakpoint,
`app.last_stop_reason == "breakpoint"`, `app.last_stop_detail` includes the
address, and `app.debug.registers()["rip"]` is the breakpoint address.

```python
emu = sogen.linux.create_empty(emulation_root="/")
emu.memory.allocate_memory_at(0x100000, 0x1000, sogen.MemoryPermission.exec)
emu.write_memory(0x100000, b"\x0f\xa2\x0f\xa2")  # cpuid; cpuid
emu.write_register(sogen.Register.rip, 0x100000)
emu.debug.set_breakpoint(0x100002)
emu.debug.continue_execution()
assert emu.last_stop_reason == "breakpoint"
```

Stepping and run control methods are `step_into()`, `step_over()` (currently the
same single-instruction primitive), `run_to(address)`, `continue_execution()`,
and `pause()`. `step_out()` uses a frame-pointer walk and raises diagnostic
`RuntimeError`s that distinguish zero RBP, an unreadable saved-return-address
slot, and a zero saved return address. The messages suggest `run_to(address)`
when an explicit destination is more reliable. Introspection methods return
Python-native data:
`registers()` returns a register dictionary; `modules()` and `threads()` return
lists of dictionaries; `disassemble(address, count_or_size)` returns instruction
dictionaries with address, bytes, mnemonic, operands, and size; and
`call_stack()` returns a bounded best-effort frame-pointer walk.

## API hooks

API hooks are registered through `app.hooks.apis`.

Use `@sogen.windows.api_call(...)` to describe calling convention and parameters.
Top-level `sogen.api_call(...)` remains as compatibility alias.

### Observe API call, then run original

```python
import ctypes
import sogen

app = sogen.windows.create_application(
    "c:/test-sample.exe",
    emulation_root="./root",
)


@sogen.windows.api_call(cc=sogen.CallingConvention.stdcall, params=[ctypes.c_uint32])
def on_sleep(call, params):
    print(f"Sleep({params[0]})")


app.hooks.apis["Sleep"] = on_sleep
app.start()
```

### Intercept API call and return custom value

```python
import sogen

app = sogen.windows.create_application(
    "c:/hook-sample.exe",
    emulation_root="./root",
)


@sogen.windows.api_call(cc=sogen.CallingConvention.stdcall, params=[])
def on_get_current_process_id(call, params):
    call.return_value = 0xC0FFEE01
    return sogen.ApiContinuation.intercept


app.hooks.apis["GetCurrentProcessId"] = on_get_current_process_id
app.start()
print(app.process.exit_status)
```

Hook keys can be either:

- bare API name, for example `"Sleep"`
- qualified module form, for example `"kernel32!Sleep"`

## Memory and state

The emulator exposes direct state access. Windows and Linux emulators both
provide `serialize_state()` / `deserialize_state(bytes)` for explicit byte
checkpoints and `save_snapshot()` / `restore_snapshot()` for an in-object
snapshot slot.

```python
import sogen

emu = sogen.linux.create_empty(emulation_root="/")
base = emu.memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
emu.write_memory(base, b"ABCD")
emu.write_register(sogen.Register.rax, 0x1234)

state = emu.serialize_state()
emu.write_memory(base, b"WXYZ")
emu.write_register(sogen.Register.rax, 0)
emu.deserialize_state(state)
assert emu.read_memory(base, 4) == b"ABCD"
assert emu.read_register(sogen.Register.rax) == 0x1234
```

Linux serialized state uses version tag `linux-emulator-state-v1` and includes
backend CPU state, executed instruction count, stop diagnostics, mapped memory
bytes and permissions, process ids, brk, argv/envp/auxv, threads, directory and
epoll caches, ELF module metadata, signal actions, and mutable vDSO metadata.
Open Linux file descriptors are serialized only for stdio, procfs/memory files,
eventfds, directory descriptors, epoll descriptors, and host files that can be
reopened by path and restored to their file offset. Live pipes, sockets, unnamed
host descriptors, and unreopenable host descriptors raise during serialization
or restore rather than being silently dropped.

Linux memory managers expose mapped-region introspection through
`app.memory.mapped_regions`, `app.memory.get_mapped_regions()`,
`app.memory.get_region_info(address)`, and `app.memory.compute_memory_stats()`.
`mapped_regions` returns `sogen.linux.MemoryRegionInfo` objects with `start`,
`length`, `permissions`, `allocation_base`, `allocation_length`, `is_reserved`,
`is_committed`, `initial_permissions`, and `kind`. Composite region permissions
include `sogen.MemoryPermission.read_exec` and
`sogen.MemoryPermission.write_exec`. Linux does not have the
Windows reserve/commit split. Each mapped region reports:

- `allocation_base == start`
- `allocation_length == length`
- `is_reserved is False`
- `is_committed is True`
- `initial_permissions == permissions`
- `kind == sogen.MemoryRegionKind.private_allocation`

`compute_memory_stats()` returns a dictionary with:

- `region_count`
- `mapped_bytes`
- `executable_bytes`

Linux memory lifecycle callbacks observe successful native memory operations:

- `app.callbacks.on_memory_allocate(address, length, permissions, committed)`
  fires after `mmap`/allocation succeeds. Linux always passes `committed=True`
  because there is no reserve-only state.
- `app.callbacks.on_memory_protect(address, length, permissions)` fires after a
  protection change succeeds.
- `app.callbacks.on_memory_release(address, length)` fires after unmap/release
  succeeds.

For checkpoint-style workflows, use snapshots:

```python
emu.save_snapshot()
# ... mutate state ...
emu.restore_snapshot()
```

## Examples

Small runnable examples:

- `examples/python/basic_usage.py` for Windows
- `examples/python/linux_true.py` for Linux

Example setup notes:

- `examples/python/README.md`

## Current limitations / expectations

- Windows bindings require an emulation root
- Windows samples in this repo assume Windows-style guest paths like `c:/...`
- Windows workflows are easiest to validate against repo sample binaries such as `test-sample.exe` and `hook-sample.exe`
- backend availability depends on platform and how Sogen was built
