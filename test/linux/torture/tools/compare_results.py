#!/usr/bin/env python3

from __future__ import annotations

import argparse
import difflib
import json
import pathlib
import re
import sys
from typing import Any


ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
HEX_RE = re.compile(r"0x[0-9a-fA-F]+")
PID_LIKE_RE = re.compile(r"\b(?P<k>pid|ppid|tid|tgid)\s*[:=]\s*-?\d+\b", re.IGNORECASE)


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def parse_path_maps(items: list[str]) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid --path-map '{item}', expected OLD=NEW")
        old, new = item.split("=", 1)
        pairs.append((old, new))
    return pairs


def normalize_text(
    text: str,
    *,
    strip_ansi: bool,
    normalize_hex: bool,
    normalize_pid_like: bool,
    path_maps: list[tuple[str, str]],
    ignore_line_regexes: list[re.Pattern[str]],
) -> str:
    out = text.replace("\r\n", "\n").replace("\r", "\n")

    if strip_ansi:
        out = ANSI_RE.sub("", out)

    if normalize_hex:
        out = HEX_RE.sub("<HEX>", out)

    if normalize_pid_like:
        out = PID_LIKE_RE.sub(lambda m: f"{m.group('k')}=<N>", out)

    for old, new in path_maps:
        out = out.replace(old, new)

    if ignore_line_regexes:
        kept: list[str] = []
        for line in out.split("\n"):
            if any(rx.search(line) for rx in ignore_line_regexes):
                continue
            kept.append(line)
        out = "\n".join(kept)

    # Normalize trailing whitespace for stable diffs.
    out = "\n".join(line.rstrip() for line in out.split("\n"))
    return out


def unified_diff(a: str, b: str, from_name: str, to_name: str) -> str:
    return "".join(
        difflib.unified_diff(
            a.splitlines(keepends=True),
            b.splitlines(keepends=True),
            fromfile=from_name,
            tofile=to_name,
            n=3,
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare native and emulated run results with normalization hooks"
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--result-json", help="Combined JSON with 'native' and 'emu' sections"
    )
    group.add_argument(
        "--pair",
        nargs=2,
        metavar=("NATIVE_JSON", "EMU_JSON"),
        help="Two standalone JSON files",
    )

    parser.add_argument(
        "--path-map",
        action="append",
        default=[],
        help="Path normalization OLD=NEW (repeatable)",
    )
    parser.add_argument(
        "--ignore-line-regex",
        action="append",
        default=[],
        help="Drop lines matching regex before comparison (repeatable)",
    )
    parser.add_argument(
        "--keep-ansi", action="store_true", help="Do not strip ANSI escape sequences"
    )
    parser.add_argument(
        "--keep-hex", action="store_true", help="Do not normalize hex addresses"
    )
    parser.add_argument(
        "--keep-pid-like",
        action="store_true",
        help="Do not normalize pid/ppid/tid/tgid values",
    )
    parser.add_argument(
        "--output", help="Write machine-readable compare summary to this path"
    )
    parser.add_argument(
        "--quiet", action="store_true", help="Suppress human-readable summary"
    )

    args = parser.parse_args()

    if args.result_json:
        combined = load_json(pathlib.Path(args.result_json))
        native = combined.get("native")
        emu = combined.get("emu")
        if native is None or emu is None:
            print(
                "error: --result-json must contain 'native' and 'emu' sections",
                file=sys.stderr,
            )
            return 2
    else:
        native = load_json(pathlib.Path(args.pair[0]))
        emu = load_json(pathlib.Path(args.pair[1]))

    path_maps = parse_path_maps(args.path_map)
    ignore_regexes = [re.compile(x) for x in args.ignore_line_regex]

    native_stdout = normalize_text(
        str(native.get("stdout", "")),
        strip_ansi=not args.keep_ansi,
        normalize_hex=not args.keep_hex,
        normalize_pid_like=not args.keep_pid_like,
        path_maps=path_maps,
        ignore_line_regexes=ignore_regexes,
    )
    emu_stdout = normalize_text(
        str(emu.get("stdout", "")),
        strip_ansi=not args.keep_ansi,
        normalize_hex=not args.keep_hex,
        normalize_pid_like=not args.keep_pid_like,
        path_maps=path_maps,
        ignore_line_regexes=ignore_regexes,
    )

    native_stderr = normalize_text(
        str(native.get("stderr", "")),
        strip_ansi=not args.keep_ansi,
        normalize_hex=not args.keep_hex,
        normalize_pid_like=not args.keep_pid_like,
        path_maps=path_maps,
        ignore_line_regexes=ignore_regexes,
    )
    emu_stderr = normalize_text(
        str(emu.get("stderr", "")),
        strip_ansi=not args.keep_ansi,
        normalize_hex=not args.keep_hex,
        normalize_pid_like=not args.keep_pid_like,
        path_maps=path_maps,
        ignore_line_regexes=ignore_regexes,
    )

    exit_match = native.get("exit_code") == emu.get("exit_code")
    timeout_match = bool(native.get("timed_out", False)) == bool(
        emu.get("timed_out", False)
    )
    stdout_match = native_stdout == emu_stdout
    stderr_match = native_stderr == emu_stderr
    all_match = exit_match and timeout_match and stdout_match and stderr_match

    summary: dict[str, Any] = {
        "match": all_match,
        "checks": {
            "exit_code": exit_match,
            "timed_out": timeout_match,
            "stdout": stdout_match,
            "stderr": stderr_match,
        },
        "native": {
            "exit_code": native.get("exit_code"),
            "timed_out": bool(native.get("timed_out", False)),
        },
        "emu": {
            "exit_code": emu.get("exit_code"),
            "timed_out": bool(emu.get("timed_out", False)),
        },
        "diff": {
            "stdout": ""
            if stdout_match
            else unified_diff(native_stdout, emu_stdout, "native.stdout", "emu.stdout"),
            "stderr": ""
            if stderr_match
            else unified_diff(native_stderr, emu_stderr, "native.stderr", "emu.stderr"),
        },
    }

    if args.output:
        out_path = pathlib.Path(args.output)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    if not args.quiet:
        print(json.dumps(summary["checks"], indent=2, sort_keys=True))
        if not all_match:
            if summary["diff"]["stdout"]:
                print("\n--- stdout diff ---")
                print(summary["diff"]["stdout"], end="")
            if summary["diff"]["stderr"]:
                print("\n--- stderr diff ---")
                print(summary["diff"]["stderr"], end="")

    return 0 if all_match else 1


if __name__ == "__main__":
    raise SystemExit(main())
