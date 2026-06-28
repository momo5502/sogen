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


def capture_output(chunks):
    def capture(data):
        if isinstance(data, str):
            chunks.append(data.encode())
        else:
            chunks.append(bytes(data))

    return capture


app = linux.create_application(
    binary,
    emulation_root=root,
    backend=backend,
    disable_logging=True,
)
app.callbacks.on_stdout = capture_output(stdout_chunks)
app.callbacks.on_stderr = capture_output(stderr_chunks)
app.start()

stdout_data = b"".join(stdout_chunks)
stderr_data = b"".join(stderr_chunks)

print(f"exit_status={app.process.exit_status}", flush=True)
print(f"backend={app.backend_name}", flush=True)
print(f"requested_backend={requested_backend}", flush=True)
print(f"executed_instructions={app.executed_instructions}", flush=True)

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
