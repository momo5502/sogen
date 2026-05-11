import importlib
import os
import sys
from pathlib import Path

sys.path.insert(0, os.getcwd())
repo_artifacts = Path(__file__).resolve().parents[2] / "build" / "release" / "artifacts"
if repo_artifacts.exists():
    sys.path.insert(0, str(repo_artifacts))

mod = importlib.import_module("sogen")
assert hasattr(mod, "WindowsEmulator")
assert hasattr(mod, "MemoryManager")
assert hasattr(mod, "ProcessContext")
assert hasattr(mod, "Thread")
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
assert hasattr(emu.callbacks, "set")
assert hasattr(emu.callbacks, "clear")
print("ok")
