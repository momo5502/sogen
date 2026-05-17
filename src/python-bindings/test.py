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
hook_sample_env = os.getenv("HOOK_SAMPLE")
assert emulator_root
assert analysis_sample

# hook-sample is built next to test-sample by default; allow override via env.
hook_sample = Path(hook_sample_env) if hook_sample_env else Path(analysis_sample).with_name("hook-sample.exe")

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
    assert call.name == "Sleep"


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
        emulation_root=emulator_root,
        path_mappings={r"C:\a.txt": mapped_file},
        port_mappings={28970: 28980},
    )

    app.callbacks.on_stdout = lambda text: print(text, end="", flush=True)
    app.hooks.apis["Sleep"] = on_sleep
    app.start()
    print(
        f"\n[test.py] exit_status={app.process.exit_status}"
        f" sleep_hits={sleep_hits['count']}"
        f" executed_instructions={app.executed_instructions}"
        f" stop_reason={app.last_stop_reason}"
        f" stop_detail={app.last_stop_detail}"
        f" current_thread_id={app.current_thread_id}"
        f" backend={app.backend_name}",
        flush=True,
    )
    assert sleep_hits["count"] > 0
    assert app.process.exit_status == 0, f"non-zero exit: {app.process.exit_status}"
    assert all(arg == 1 for arg in sleep_hits["args"])

    app = None
    gc.collect()

# ----- intercept smoke test against a controlled minimal sample -----
#
# hook-sample.exe returns 0 only when GetCurrentProcessId() returns the magic
# value below. The runtime never sees a real PID because our hook intercepts
# the call. This isolates the intercept code path from any DLL behavior that
# may rely on a genuine PID.

EXPECTED_PID = 0xC0FFEE01
hook_pid_hits = {"count": 0}


@mod.api_call(cc=mod.CallingConvention.stdcall, params=[])
def on_hook_sample_pid(call, params):
    hook_pid_hits["count"] += 1
    assert call.name == "GetCurrentProcessId"
    call.return_value = EXPECTED_PID
    return mod.ApiContinuation.intercept


if hook_sample.exists():
    filesys_root = Path(emulator_root) / "filesys" / "c"
    filesys_root.mkdir(parents=True, exist_ok=True)
    hook_sample_copy = filesys_root / "hook-sample.exe"
    hook_sample_copy.write_bytes(hook_sample.read_bytes())

    hook_app = mod.create_application(
        r"C:\hook-sample.exe",
        emulation_root=emulator_root,
    )
    hook_app.callbacks.on_stdout = lambda text: print(text, end="", flush=True)
    hook_app.hooks.apis["GetCurrentProcessId"] = on_hook_sample_pid
    hook_app.start()
    print(
        f"[test.py:hook-sample] exit_status={hook_app.process.exit_status}"
        f" pid_hits={hook_pid_hits['count']}"
        f" executed_instructions={hook_app.executed_instructions}"
        f" stop_reason={hook_app.last_stop_reason}",
        flush=True,
    )
    assert hook_pid_hits["count"] >= 1, "GetCurrentProcessId hook never fired"
    assert hook_app.process.exit_status == 0, (
        f"hook-sample exited {hook_app.process.exit_status}; intercept did not redirect to {EXPECTED_PID:#x} "
        f"(pid_hits={hook_pid_hits['count']}, stop_reason={hook_app.last_stop_reason}, "
        f"stop_detail={hook_app.last_stop_detail!r}, executed={hook_app.executed_instructions})"
    )
    hook_app = None
    gc.collect()
else:
    print(f"[test.py] hook-sample not found at {hook_sample}; skipping intercept smoke", flush=True)

emu = None
mod = None

gc.collect()
print("ok")
