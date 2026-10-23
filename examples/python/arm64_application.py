"""Run a Windows ARM64 executable with the Sogen Python bindings.

Set EMULATOR_ARM64_ROOT to an ARM64 emulation root (created with create-root.bat on Windows ARM64),
or place it at ./root-arm64. Pass the guest executable and its arguments, for example:

    python examples/python/arm64_application.py c:/hello_crt.exe one two
"""

from __future__ import annotations

import os
import sys
from collections import Counter

import sogen


EMULATOR_ARM64_ROOT = os.getenv("EMULATOR_ARM64_ROOT")


def main() -> None:
    application = sys.argv[1] if len(sys.argv) > 1 else "c:/hello_crt.exe"
    app = sogen.windows.create_application(application, sys.argv[2:], emulation_root=EMULATOR_ARM64_ROOT or "./root-arm64")

    syscalls: Counter[str] = Counter()
    app.callbacks.on_stdout = lambda text: print(text, end="")
    app.callbacks.on_module_load = lambda module: print(f"loaded: {module.name} @ 0x{module.entry_point:x}")
    app.callbacks.on_syscall = lambda syscall_id, name: syscalls.update([name])
    app.start()

    print("exit status:", hex(app.process.exit_status))
    print("TEB (x18):", hex(app.read_register(sogen.arm64.Register.x18)))
    print("syscalls:", sum(syscalls.values()), "most frequent:", syscalls.most_common(5))


if __name__ == "__main__":
    main()
