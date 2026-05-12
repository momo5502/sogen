"""Small Sogen Python bindings example."""

from __future__ import annotations

import os
from pathlib import Path

import sogen


EMULATOR_ROOT = os.getenv("EMULATOR_ROOT")


def main() -> None:
    emulation_root = EMULATOR_ROOT or "./root"

    app = sogen.create_application(
        "c:/test-sample.exe",
        None,
        emulation_root=emulation_root,
        path_mappings={"c:/a.txt": Path("./sogen-example.txt")},
        port_mappings={28970: 28980},
    )

    app.callbacks.on_stdout = lambda text: print(text, end="")
    app.callbacks.on_module_load = lambda module: print(f"loaded: {module.name} @ 0x{module.entry_point:x}")
    app.start()
    print("exit status:", app.process.exit_status)


if __name__ == "__main__":
    main()
