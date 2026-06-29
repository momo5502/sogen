import gc
import os
import sys
from pathlib import Path

sys.path.insert(0, os.getcwd())
repo_artifacts = Path(__file__).resolve().parents[2] / "build" / "release" / "artifacts"
if repo_artifacts.exists():
    sys.path.insert(0, str(repo_artifacts))

import sogen

assert sogen.create_application is sogen.windows.create_application

linux = sogen.linux
assert linux.create_application is not sogen.create_application

binary = os.getenv("LINUX_PYTHON_TEST_BINARY", "/bin/true")
root = os.getenv("LINUX_PYTHON_TEST_ROOT", "/")
requested_backend = os.getenv("LINUX_PYTHON_TEST_BACKEND", "unicorn")

backends = {
    "unicorn": (sogen.Backend.unicorn, "Unicorn Engine"),
    "kvm": (sogen.Backend.kvm, "Linux KVM"),
}
assert requested_backend in backends, f"unsupported backend: {requested_backend!r}"
backend, expected_backend_name = backends[requested_backend]

stdout_chunks = []
stderr_chunks = []
syscall_events = []


def capture_output(chunks):
    def capture(data):
        if isinstance(data, str):
            chunks.append(data.encode())
        else:
            chunks.append(bytes(data))

    return capture

def capture_syscall(syscall_id, syscall_name):
    assert isinstance(syscall_id, int), f"syscall id is not int: {syscall_id!r}"
    assert syscall_id >= 0, f"negative syscall id: {syscall_id!r}"
    assert isinstance(syscall_name, str), f"syscall name is not str: {syscall_name!r}"
    assert syscall_name, "empty syscall name"
    syscall_events.append((syscall_id, syscall_name))
    if len(syscall_events) == 1:
        return sogen.HookContinuation.run
    return None


app = linux.create_application(
    binary,
    emulation_root=root,
    backend=backend,
    disable_logging=True,
)
assert hasattr(app.callbacks, "on_syscall")
app.callbacks.on_syscall = capture_syscall
app.callbacks.clear("on_syscall")
app.callbacks.set("on_syscall", capture_syscall)
app.callbacks.clear("syscall")
app.callbacks.on_stdout = capture_output(stdout_chunks)
app.callbacks.on_stderr = capture_output(stderr_chunks)
app.callbacks.set("syscall", capture_syscall)
app.start()

stdout_data = b"".join(stdout_chunks)
stderr_data = b"".join(stderr_chunks)

syscall_names = [name for _, name in syscall_events]

print(f"exit_status={app.process.exit_status}", flush=True)
print(f"backend={app.backend_name}", flush=True)
print(f"requested_backend={requested_backend}", flush=True)
print(f"executed_instructions={app.executed_instructions}", flush=True)
print(f"syscall_count={len(syscall_events)}", flush=True)
print(f"syscall_names={','.join(syscall_names[:20])}", flush=True)


assert syscall_events, "syscall callback did not fire"
assert "exit_group" in syscall_names or "exit" in syscall_names, (
    f"exit syscall not observed: {syscall_names!r}"
)
assert app.process.exit_status == 0, f"non-zero exit: {app.process.exit_status}"
assert stdout_data == b"", f"unexpected stdout: {stdout_data!r}"
assert stderr_data == b"", f"unexpected stderr: {stderr_data!r}"
assert app.backend_name == expected_backend_name, (
    f"backend name {app.backend_name!r} != {expected_backend_name!r}"
)
if requested_backend == "unicorn":
    assert app.executed_instructions > 0, "unicorn did not execute any instructions"

app = None
gc.collect()
