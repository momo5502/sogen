<p align="center">
  <a href="https://github.com/momo5502/sogen"><img src="https://momo5502.com/sogen/banner.png" height="220" alt="Sogen" /></a>
</p>

Sogen exposes Python bindings for its Windows user-space emulator. The Python API is meant for scripting runs, building small analysis helpers, and quickly iterating on callbacks and hooks without rebuilding C++.

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

app = sogen.create_application(
    "c:/test-sample.exe",
    None,
    emulation_root="./root",
)

app.callbacks.on_stdout = lambda text: print(text, end="")
app.start()
print("exit status:", app.process.exit_status)
```

## Minimal example with file + port mappings

```python
from pathlib import Path
import sogen

app = sogen.create_application(
    "c:/test-sample.exe",
    None,
    emulation_root="./root",
    path_mappings={"c:/a.txt": Path("./a.txt")},
    port_mappings={28970: 28980},
)

app.callbacks.on_stdout = lambda text: print(text, end="")
app.start()
print("exit status:", app.process.exit_status)
```

## Choosing backend

`create_empty()` and `create_application()` accept an explicit backend.

```python
import sogen

emu = sogen.create_empty(
    emulation_root="./root",
    backend=sogen.Backend.unicorn,
)
```

Available values:

- `sogen.Backend.unicorn`
- `sogen.Backend.icicle`
- `sogen.Backend.whp`

Default is `sogen.Backend.unicorn`.

## High-level structure

Main entry points:

- `sogen.create_empty(...)`
- `sogen.create_application(...)`

Common objects exposed by the bindings:

- `WindowsEmulator`
- `ProcessContext`
- `Thread`
- `MemoryManager`
- `Hooks`
- `Callbacks`

Common things you will do:

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

app = sogen.create_application(
    "c:/test-sample.exe",
    None,
    emulation_root="./root",
)


def on_module_load(module):
    print(f"loaded {module.name} @ 0x{module.entry_point:x}")


app.callbacks.on_module_load = on_module_load
app.start()
```

Useful callback slots include:

- `app.callbacks.on_stdout`
- `app.callbacks.on_syscall`
- `app.callbacks.on_memory_violate`
- `app.callbacks.on_module_load`
- `app.callbacks.on_module_unload`

## API hooks

API hooks are registered through `app.hooks.apis`.

Use `@sogen.api_call(...)` to describe calling convention and parameters.

### Observe API call, then run original

```python
import ctypes
import sogen

app = sogen.create_application(
    "c:/test-sample.exe",
    None,
    emulation_root="./root",
)


@sogen.api_call(cc=sogen.CallingConvention.stdcall, params=[ctypes.c_uint32])
def on_sleep(call, params):
    print(f"Sleep({params[0]})")
    return sogen.ApiContinuation.run_original


app.hooks.apis["Sleep"] = on_sleep
app.start()
```

### Intercept API call and return custom value

```python
import sogen

app = sogen.create_application(
    "c:/hook-sample.exe",
    None,
    emulation_root="./root",
)


@sogen.api_call(cc=sogen.CallingConvention.stdcall, params=[])
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

The emulator exposes direct state access.

```python
import sogen

emu = sogen.create_empty(emulation_root="./root")
base = emu.memory.allocate_memory(0x1000, sogen.MemoryPermission.read_write)
emu.write_memory(base, b"ABCD")
print(emu.read_memory(base, 4))

state = emu.serialize_state()
emu.write_memory(base, b"WXYZ")
emu.deserialize_state(state)
print(emu.read_memory(base, 4))
```

For checkpoint-style workflows, use snapshots:

```python
emu.save_snapshot()
# ... mutate state ...
emu.restore_snapshot()
```

## Examples

Small runnable example:

- `examples/python/basic_usage.py`

Example setup notes:

- `examples/python/README.md`

## Current limitations / expectations

- bindings require an emulation root
- samples in this repo assume Windows-style guest paths like `c:/...`
- some workflows are easiest to validate against repo sample binaries such as `test-sample.exe` and `hook-sample.exe`
- backend availability depends on platform and how Sogen was built

## Suggested next docs

If this Python API becomes larger, good follow-up pages would be:

- installation and platform notes
- quickstart against repo samples
- API hooks guide
- callbacks guide
- memory/register guide
- snapshots and serialization guide
- backend selection guide
