import ctypes
import gc
import importlib
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.getcwd())
repo_artifacts = Path(__file__).resolve().parents[2] / "build" / "release" / "artifacts"
if repo_artifacts.exists():
    sys.path.insert(0, str(repo_artifacts))

emulator_root = os.getenv("EMULATOR_ROOT")
analysis_sample = os.getenv("ANALYSIS_SAMPLE")
assert emulator_root
assert analysis_sample

mod = importlib.import_module("sogen")
assert hasattr(mod, "WindowsEmulator")
assert hasattr(mod, "MemoryManager")
assert hasattr(mod, "ProcessContext")
assert hasattr(mod, "Thread")
assert hasattr(mod, "Hook")
assert hasattr(mod, "Hooks")
assert hasattr(mod, "Instruction")
assert hasattr(mod, "HookContinuation")
assert hasattr(mod, "MemoryViolationContinuation")
assert hasattr(mod, "ApiContinuation")
assert hasattr(mod, "CallingConvention")
assert hasattr(mod, "api_call")
assert hasattr(mod, "Backend")
assert hasattr(mod, "BasicBlock")
assert hasattr(mod, "MappedModule")
assert hasattr(mod, "Register")
assert hasattr(mod, "MemoryPermission")
assert hasattr(mod, "MemoryViolationType")
assert hasattr(mod, "create_empty")
assert hasattr(mod, "create_application")

emu = mod.create_empty(emulation_root=emulator_root)
assert emu.backend_name in {"Unicorn Engine", "icicle-emu", "Windows Hypervisor Platform"}
assert hasattr(emu, "read_memory")
assert hasattr(emu, "write_memory")
assert hasattr(emu, "read_register")
assert hasattr(emu, "write_register")
assert hasattr(emu, "callbacks")
assert hasattr(emu, "hooks")
assert hasattr(emu.hooks, "apis")
assert hasattr(emu.process, "callbacks")
assert hasattr(emu.callbacks, "on_stdout")
assert hasattr(emu.callbacks, "on_syscall")
assert hasattr(emu.callbacks, "on_memory_violate")
assert hasattr(emu.callbacks, "on_module_load")
assert hasattr(emu.callbacks, "on_module_unload")
assert hasattr(emu.callbacks, "set")
assert hasattr(emu.callbacks, "clear")
assert hasattr(emu.hooks, "memory_execution")
assert hasattr(emu.hooks, "memory_read")
assert hasattr(emu.hooks, "instruction")
assert hasattr(emu, "save_snapshot")
assert hasattr(emu, "restore_snapshot")
assert hasattr(emu, "serialize_state")
assert hasattr(emu, "deserialize_state")
emu.callbacks.on_stdout = lambda s: None
emu.process.callbacks.on_thread_create = lambda h, t: None
emu.callbacks.on_module_load = lambda m: None
emu.callbacks.on_module_unload = lambda m: None

sleep_hits = {"count": 0, "args": []}

@mod.api_call(cc=mod.CallingConvention.stdcall, params=[ctypes.c_uint32])
def on_sleep(call, params):
    sleep_hits["count"] += 1
    sleep_hits["args"].append(params[0])
    return mod.ApiContinuation.run_original

state_base = emu.memory.allocate_memory(0x1000, mod.MemoryPermission.read_write)
emu.write_memory(state_base, b"ABCD")
state_bytes = emu.serialize_state()
emu.write_memory(state_base, b"WXYZ")
emu.deserialize_state(state_bytes)
assert emu.read_memory(state_base, 4) == b"ABCD"
emu.save_snapshot()
emu.write_memory(state_base, b"1234")
emu.restore_snapshot()
assert emu.read_memory(state_base, 4) == b"ABCD"

test_sample = Path(analysis_sample)
assert test_sample.exists()

with tempfile.TemporaryDirectory(prefix="sogen-python-") as temp_dir:
    mapped_file = Path(temp_dir) / "a.txt"
    filesys_root = Path(emulator_root) / "filesys" / "c"
    filesys_root.mkdir(parents=True, exist_ok=True)
    sample_copy = filesys_root / "test-sample.exe"
    if not sample_copy.exists():
        sample_copy.write_bytes(test_sample.read_bytes())

    app = mod.create_application(
        r"C:\test-sample.exe",
        None,
        emulation_root=emulator_root,
        path_mappings={r"C:\a.txt": mapped_file},
        port_mappings={28970: 28980},
    )

    app.hooks.apis["Sleep"] = on_sleep
    app.start()
    assert sleep_hits["count"] > 0
    assert app.process.exit_status == 0
    assert all(arg == 1 for arg in sleep_hits["args"])

    app = None
    gc.collect()

emu = None
mod = None

gc.collect()
print("ok")
