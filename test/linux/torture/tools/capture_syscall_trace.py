#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_ANALYZER = REPO_ROOT / "build" / "debug" / "artifacts" / "linux-analyzer"
DEFAULT_OUTPUT = (
    REPO_ROOT
    / "test"
    / "linux"
    / "torture"
    / "artifacts"
    / "trace"
    / "syscall_trace.json"
)

TRACE_CLASS_SYSCALLS: dict[str, list[str]] = {
    "loader": [
        "execve",
        "mmap",
        "mprotect",
        "openat",
        "newfstatat",
        "read",
        "close",
        "brk",
        "arch_prctl",
    ],
    "file": [
        "open",
        "openat",
        "read",
        "write",
        "close",
        "newfstatat",
        "lseek",
        "getdents64",
        "faccessat",
        "unlinkat",
    ],
    "io": [
        "poll",
        "ppoll",
        "select",
        "pselect6",
        "epoll_create1",
        "epoll_ctl",
        "epoll_wait",
        "pipe",
        "pipe2",
        "ioctl",
    ],
    "memory": ["mmap", "mprotect", "munmap", "mremap", "madvise", "brk"],
    "process": [
        "exit",
        "exit_group",
        "getpid",
        "getppid",
        "gettid",
        "clone",
        "wait4",
        "prctl",
    ],
    "time": [
        "time",
        "clock_gettime",
        "clock_getres",
        "gettimeofday",
        "nanosleep",
        "clock_nanosleep",
    ],
    "signal": [
        "rt_sigaction",
        "rt_sigprocmask",
        "sigaltstack",
        "rt_sigreturn",
        "kill",
        "tgkill",
    ],
    "thread": [
        "clone",
        "futex",
        "set_tid_address",
        "set_robust_list",
        "sched_yield",
        "sched_getaffinity",
    ],
    "socket": [
        "socket",
        "connect",
        "accept",
        "accept4",
        "sendto",
        "recvfrom",
        "bind",
        "listen",
        "shutdown",
    ],
}


def resolve_classes(class_args: list[str]) -> list[str]:
    if not class_args:
        return ["loader", "file", "memory", "process", "time"]

    out: list[str] = []
    for item in class_args:
        if item == "all":
            return sorted(TRACE_CLASS_SYSCALLS.keys())
        if item not in TRACE_CLASS_SYSCALLS:
            raise ValueError(f"Unknown trace class: {item}")
        if item not in out:
            out.append(item)
    return out


def flatten_syscalls(classes: list[str]) -> list[str]:
    names: list[str] = []
    for c in classes:
        for s in TRACE_CLASS_SYSCALLS[c]:
            if s not in names:
                names.append(s)
    return names


def run_capture(
    command: list[str], *, cwd: str | None, env: dict[str, str], timeout_sec: float
) -> dict[str, Any]:
    t0 = time.perf_counter()
    try:
        cp = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            check=False,
        )
        return {
            "status": "ok",
            "exit_code": cp.returncode,
            "timed_out": False,
            "stdout": cp.stdout,
            "stderr": cp.stderr,
            "duration_ms": int((time.perf_counter() - t0) * 1000),
        }
    except subprocess.TimeoutExpired as ex:
        return {
            "status": "timeout",
            "exit_code": None,
            "timed_out": True,
            "stdout": (
                ex.stdout.decode("utf-8", errors="replace")
                if isinstance(ex.stdout, bytes)
                else (ex.stdout or "")
            ),
            "stderr": (
                ex.stderr.decode("utf-8", errors="replace")
                if isinstance(ex.stderr, bytes)
                else (ex.stderr or "")
            ),
            "duration_ms": int((time.perf_counter() - t0) * 1000),
        }


def parse_strace_lines(lines: list[str]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    rx = re.compile(r"^(?P<name>[a-zA-Z0-9_]+)\((?P<args>.*)\)\s+=\s+(?P<ret>.+)$")
    for idx, line in enumerate(lines):
        m = rx.match(line.strip())
        if m:
            events.append(
                {
                    "index": idx,
                    "syscall": m.group("name"),
                    "args": m.group("args"),
                    "ret": m.group("ret"),
                    "raw": line.rstrip("\n"),
                }
            )
        elif line.strip():
            events.append({"index": idx, "raw": line.rstrip("\n")})
    return events


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture syscall traces for deterministic torture classes"
    )
    parser.add_argument("--mode", choices=["native", "emu"], required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument(
        "--root", default=str(REPO_ROOT), help="Emulation root for --mode emu"
    )
    parser.add_argument(
        "--analyzer",
        default=str(DEFAULT_ANALYZER),
        help="linux-analyzer path for --mode emu",
    )
    parser.add_argument(
        "--class",
        dest="classes",
        action="append",
        default=[],
        help="Trace class (repeatable); use 'all' for every class",
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--cwd")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("binary_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    binary = pathlib.Path(args.binary).resolve()
    root = pathlib.Path(args.root).resolve()
    analyzer = pathlib.Path(args.analyzer).resolve()
    output = pathlib.Path(args.output).resolve()

    classes = resolve_classes(args.classes)
    syscall_names = flatten_syscalls(classes)

    binary_args = list(args.binary_args)
    if binary_args and binary_args[0] == "--":
        binary_args = binary_args[1:]

    env = os.environ.copy()
    result: dict[str, Any] = {
        "schema_version": 1,
        "mode": args.mode,
        "binary": str(binary),
        "binary_args": binary_args,
        "classes": classes,
        "syscall_filter": syscall_names,
        "trace_source": None,
        "events": [],
        "raw_trace_files": [],
        "note": None,
    }

    if args.mode == "native":
        strace = shutil.which("strace")
        if strace and platform.system().lower() == "linux":
            trace_prefix = output.parent / (output.stem + ".native.strace")
            cmd = [
                strace,
                "-ff",
                "-qq",
                "-tt",
                "-T",
                "-e",
                "trace=" + ",".join(syscall_names),
                "-o",
                str(trace_prefix),
                str(binary),
            ] + binary_args
            capture = run_capture(cmd, cwd=args.cwd, env=env, timeout_sec=args.timeout)
            result.update(capture)
            result["trace_source"] = "strace"

            trace_files = sorted(trace_prefix.parent.glob(trace_prefix.name + "*"))
            result["raw_trace_files"] = [str(p) for p in trace_files]

            lines: list[str] = []
            for p in trace_files:
                lines.extend(
                    p.read_text(encoding="utf-8", errors="replace").splitlines(True)
                )
            result["events"] = parse_strace_lines(lines)
        else:
            cmd = [str(binary)] + binary_args
            capture = run_capture(cmd, cwd=args.cwd, env=env, timeout_sec=args.timeout)
            result.update(capture)
            result["trace_source"] = "none"
            result["note"] = (
                "strace unavailable or host is not Linux; executed binary without syscall-level capture"
            )
    else:
        cmd = [
            str(analyzer),
            "--verbose",
            "--root",
            str(root),
            str(binary),
        ] + binary_args
        capture = run_capture(cmd, cwd=args.cwd, env=env, timeout_sec=args.timeout)
        result.update(capture)
        result["trace_source"] = "linux-analyzer-verbose"

        merged = (capture.get("stdout") or "") + "\n" + (capture.get("stderr") or "")
        events: list[dict[str, Any]] = []

        for idx, line in enumerate(merged.splitlines()):
            if "syscall" in line.lower() or "Unimplemented syscall" in line:
                events.append({"index": idx, "raw": line})

        result["events"] = events
        if not events:
            result["note"] = (
                "No structured syscall events found in verbose output; use native mode with strace on Linux for authoritative traces"
            )

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    print(
        json.dumps(
            {"trace_json": str(output), "event_count": len(result.get("events", []))},
            sort_keys=True,
        )
    )
    return 0 if result.get("status") == "ok" else 2


if __name__ == "__main__":
    raise SystemExit(main())
