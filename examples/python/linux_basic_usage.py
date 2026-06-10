"""sogen.linux Tier 3 example: path mappings, port mappings, rich syscall info."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

import sogen


def build_hello_binary() -> Path:
    source = Path(__file__).resolve().parents[2] / "deps" / "sogen-linux-files" / "test" / "linux" / "hello.S"
    with tempfile.TemporaryDirectory(prefix="sogen-linux-example-") as tmp:
        out = Path(tmp) / "hello"
        obj = Path(tmp) / "hello.o"
        subprocess.check_call(["as", "-64", "-o", str(obj), str(source)])
        subprocess.check_call(["ld", "-o", str(out), str(obj)])
        data = out.read_bytes()
    dest = Path(tempfile.gettempdir()) / "sogen-linux-hello"
    dest.write_bytes(data)
    dest.chmod(0o755)
    return dest


def main() -> None:
    binary = os.getenv("LINUX_SAMPLE") or str(build_hello_binary())
    emulation_root = os.getenv("LINUX_EMULATION_ROOT", "")

    with tempfile.TemporaryDirectory(prefix="sogen-linux-tier3-") as tmp:
        tmp_path = Path(tmp)
        mapped_file = tmp_path / "secret.txt"
        mapped_file.write_text("mapped-content\n")

        stdout_lines: list[str] = []
        syscalls: list[str] = []

        app = sogen.linux.create_application(
            binary,
            emulation_root=emulation_root,
            env={"PATH": "/usr/bin:/bin", "HOME": "/root", "TERM": "xterm"},
            path_mappings={"/tmp/secret.txt": mapped_file},
            port_mappings={28970: 28980},
            verbose=False,
        )

        app.callbacks.on_stdout = lambda text: stdout_lines.append(text)

        def on_syscall(info: sogen.linux.SyscallInfo) -> sogen.linux.SyscallContinuation:
            syscalls.append(f"{info.name}({list(info.args)})")
            return sogen.linux.SyscallContinuation.run_handler

        app.callbacks.on_syscall = on_syscall

        assert app.get_host_port(28970) == 28980
        assert app.get_emulator_port(28980) == 28970

        app.start()
        print("stdout:", "".join(stdout_lines).rstrip())
        print("syscalls:", syscalls)
        print("exit status:", app.process.exit_status)


if __name__ == "__main__":
    main()
