import importlib

mod = importlib.import_module("sogen")
assert hasattr(mod, "WindowsEmulator")
assert hasattr(mod, "create_empty")
assert hasattr(mod, "create_application")
print("ok")
