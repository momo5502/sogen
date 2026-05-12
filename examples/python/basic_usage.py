"""Small Sogen Python bindings example."""

from __future__ import annotations

import os
from pathlib import Path

import sogen


EMULATOR_ROOT = os.getenv("EMULATOR_ROOT")
ANALYSIS_SAMPLE = os.getenv("ANALYSIS_SAMPLE")


def main() -> None:
    emu = sogen.create_empty(
        emulation_root=EMULATOR_ROOT or r"C:\sogen-root",
        backend=sogen.Backend.unicorn,
    )

    print("backend:", emu.backend_name)
    print("executed instructions:", emu.executed_instructions)

    if not (EMULATOR_ROOT and ANALYSIS_SAMPLE):
        return

    app = sogen.create_application(
        r"C:\test-sample.exe",
        None,
        emulation_root=EMULATOR_ROOT,
        path_mappings={r"C:\a.txt": Path(os.getenv("TEMP", ".")) / "sogen-example.txt"},
        port_mappings={28970: 28980},
    )

    entry_point = {"value": None}
    entry_hook = {"value": None}

    def on_module_load(module: sogen.MappedModule) -> None:
        if module.name.lower() != Path(ANALYSIS_SAMPLE).name.lower():
            return
        if entry_point["value"] is None:
            entry_point["value"] = module.entry_point
            entry_hook["value"] = app.hooks.memory_execution_at(
                module.entry_point,
                lambda address: print(f"hit entry point: 0x{address:x}"),
            )

    app.callbacks.on_module_load = on_module_load
    app.callbacks.on_stdout = lambda text: print(text, end="")

    app.start()
    print("exit status:", app.process.exit_status)


if __name__ == "__main__":
    main()
