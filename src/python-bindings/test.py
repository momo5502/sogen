import importlib
import os
import sys

sys.path.insert(0, os.getcwd())

mod = importlib.import_module("sogen")
assert hasattr(mod, "WindowsEmulator")
assert hasattr(mod, "MemoryManager")
assert hasattr(mod, "ProcessContext")
assert hasattr(mod, "Thread")
assert hasattr(mod, "Register")
assert hasattr(mod, "MemoryPermission")
assert hasattr(mod, "create_empty")
assert hasattr(mod, "create_application")

emu = mod.create_empty()
assert hasattr(emu, "read_memory")
assert hasattr(emu, "write_memory")
assert hasattr(emu, "read_register")
assert hasattr(emu, "write_register")
assert hasattr(emu, "set_callback")
print("ok")
