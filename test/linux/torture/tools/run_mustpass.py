#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_BASELINE = REPO_ROOT / "test" / "linux" / "torture" / "baseline.mustpass.json"
DEFAULT_RUNNER = pathlib.Path(__file__).resolve().parent / "run_native_vs_emu.py"
DEFAULT_ANALYZER = REPO_ROOT / "build" / "debug" / "artifacts" / "linux-analyzer"
DEFAULT_ARTIFACTS = REPO_ROOT / "test" / "linux" / "torture" / "artifacts"


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def resolve_path(base: pathlib.Path, raw: str) -> pathlib.Path:
    p = pathlib.Path(raw)
    if p.is_absolute():
        return p
    return (base / p).resolve()


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Phase 10 must-pass torture slice")
    parser.add_argument(
        "--baseline",
        default=str(DEFAULT_BASELINE),
        help="Path to baseline.mustpass.json",
    )
    parser.add_argument(
        "--runner", default=str(DEFAULT_RUNNER), help="Path to run_native_vs_emu.py"
    )
    parser.add_argument(
        "--analyzer", default=str(DEFAULT_ANALYZER), help="Path to linux-analyzer"
    )
    parser.add_argument("--root", default=str(REPO_ROOT), help="Emulation root")
    parser.add_argument(
        "--artifacts-dir", default=str(DEFAULT_ARTIFACTS), help="Artifacts directory"
    )
    parser.add_argument("--timeout", type=float, default=120.0, help="Per-test timeout")
    args = parser.parse_args()

    baseline_path = pathlib.Path(args.baseline).resolve()
    runner_path = pathlib.Path(args.runner).resolve()
    analyzer_path = pathlib.Path(args.analyzer).resolve()
    emu_root = pathlib.Path(args.root).resolve()
    artifacts_dir = pathlib.Path(args.artifacts_dir).resolve()
    mustpass_artifacts = artifacts_dir / "mustpass"
    mustpass_artifacts.mkdir(parents=True, exist_ok=True)

    baseline = load_json(baseline_path)
    tests = baseline.get("tests", [])
    if not isinstance(tests, list) or not tests:
        print("error: baseline has no tests", file=sys.stderr)
        return 2

    failed = 0
    for entry in tests:
        name = str(entry["name"])
        binary = resolve_path(REPO_ROOT, str(entry["binary"]))
        binary_args = [str(x) for x in entry.get("args", [])]
        expected_exit = int(entry.get("expected_exit", 0))
        expected_out = [str(x) for x in entry.get("stdout_contains", [])]

        result_json = mustpass_artifacts / f"{name}.result.json"

        cmd = [
            sys.executable,
            str(runner_path),
            "--mode",
            "emu",
            "--binary",
            str(binary),
            "--analyzer",
            str(analyzer_path),
            "--root",
            str(emu_root),
            "--timeout",
            str(args.timeout),
            "--test-name",
            name,
            "--artifacts-dir",
            str(artifacts_dir),
            "--output",
            str(result_json),
        ]

        if binary_args:
            cmd.append("--")
            cmd.extend(binary_args)

        cp = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if cp.stdout:
            sys.stdout.write(cp.stdout)
        if cp.stderr:
            sys.stderr.write(cp.stderr)

        if not result_json.exists():
            failed += 1
            print(f"TEST:{name}:FAIL:missing result.json")
            continue

        result = load_json(result_json)
        emu = result.get("emu") or {}
        exit_code = emu.get("exit_code")
        stdout = str(emu.get("stdout", ""))

        reasons: list[str] = []
        if exit_code != expected_exit:
            reasons.append(f"exit={exit_code} expected={expected_exit}")

        for token in expected_out:
            if token not in stdout:
                reasons.append(f"missing_stdout_token={token!r}")

        if reasons:
            failed += 1
            print(f"TEST:{name}:FAIL:{'; '.join(reasons)}")
        else:
            print(f"TEST:{name}:PASS:must-pass")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
