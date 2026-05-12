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
emu.callbacks.on_stdout = lambda s: None
emu.process.callbacks.on_thread_create = lambda h, t: None
emu.callbacks.on_module_load = lambda m: None
emu.callbacks.on_module_unload = lambda m: None

test_sample = Path(analysis_sample)
assert test_sample.exists()
counts = {"blocks": 0, "syscalls": 0, "entry_point": 0}
main_entry_point = {"value": None}
entry_hook = {"value": None}

with tempfile.TemporaryDirectory(prefix="sogen-python-") as temp_dir:
    mapped_file = Path(temp_dir) / "a.txt"
    app = mod.create_application(
        r"C:\test-sample.exe",
        None,
        emulation_root=emulator_root,
        path_mappings={r"C:\a.txt": mapped_file},
        port_mappings={28970: 28980},
    )

    block_hook = app.hooks.basic_block(lambda block: counts.__setitem__("blocks", counts["blocks"] + 1))
    app.callbacks.on_syscall = lambda syscall_id, name: (
        counts.__setitem__("syscalls", counts["syscalls"] + 1) or mod.HookContinuation.run
    )

    def on_module_load(module):
        if module.name.lower() != test_sample.name.lower():
            return
        if main_entry_point["value"] is None:
            main_entry_point["value"] = module.entry_point
            entry_hook["value"] = app.hooks.memory_execution_at(
                module.entry_point,
                lambda address: counts.__setitem__("entry_point", counts["entry_point"] + 1),
            )

    app.callbacks.on_module_load = on_module_load

    app.start()
    assert app.process.exit_status is not None
    assert counts["blocks"] > 0
    assert counts["syscalls"] > 0
    assert main_entry_point["value"] is not None
    assert counts["entry_point"] > 0
    assert entry_hook["value"] is not None
    assert entry_hook["value"].active
    assert block_hook.active
    entry_hook["value"].remove()
    block_hook.remove()
    assert not entry_hook["value"].active
    assert not block_hook.active

del block_hook
emu = None
app = None
mod = None
import gc

gc.collect()
print("ok")
