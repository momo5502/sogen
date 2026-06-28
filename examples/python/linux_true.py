"""Run /bin/true with Sogen Linux Python bindings."""

from __future__ import annotations

import os

import sogen


LINUX_EMULATION_ROOT = os.getenv("LINUX_EMULATION_ROOT", "/")
LINUX_TRUE = os.getenv("LINUX_TRUE", "/bin/true")


def main() -> int:
    app = sogen.linux.create_application(
        LINUX_TRUE,
        emulation_root=LINUX_EMULATION_ROOT,
        backend=sogen.Backend.unicorn,
    )

    app.start()
    print("exit status:", app.process.exit_status)
    print("executed instructions:", app.executed_instructions)
    return app.process.exit_status if app.process.exit_status is not None else 1


if __name__ == "__main__":
    raise SystemExit(main())
