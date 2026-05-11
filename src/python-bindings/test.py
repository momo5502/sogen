import importlib
import os
import sys
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
assert hasattr(mod, "BasicBlock")
assert hasattr(mod, "MappedModule")
assert hasattr(mod, "Register")
assert hasattr(mod, "MemoryPermission")
assert hasattr(mod, "MemoryViolationType")
assert hasattr(mod, "create_empty")
assert hasattr(mod, "create_application")

emu = mod.create_empty(registry_directory=str(repo_artifacts / "registry"))
assert hasattr(emu, "read_memory")
assert hasattr(emu, "write_memory")
assert hasattr(emu, "read_register")
assert hasattr(emu, "write_register")
assert hasattr(emu, "callbacks")
assert hasattr(emu, "hooks")
assert hasattr(emu.process, "callbacks")
assert hasattr(emu.callbacks, "on_stdout")
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
app = mod.create_application(r"C:\test-sample.exe", None, emulation_root=emulator_root)
app.start()
assert app.process.exit_status is not None

emu = None
app = None
mod = None
import gc

gc.collect()
print("ok")
