#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_ANALYZER = REPO_ROOT / "build" / "debug" / "artifacts" / "linux-analyzer"
DEFAULT_ARTIFACTS = REPO_ROOT / "test" / "linux" / "torture" / "artifacts"
DEFAULT_COMPARE_SCRIPT = pathlib.Path(__file__).resolve().parent / "compare_results.py"


def utc_now_iso() -> str:
    return (
        dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )


def to_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def parse_env_overrides(items: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid env override '{item}', expected KEY=VALUE")
        k, v = item.split("=", 1)
        env[k] = v
    return env


def ensure_artifact_layout(base: pathlib.Path) -> None:
    base.mkdir(parents=True, exist_ok=True)
    for name in ("seed", "binary", "root", "trace"):
        (base / name).mkdir(parents=True, exist_ok=True)


def run_command(
    command: list[str], *, cwd: str | None, env: dict[str, str], timeout_sec: float
) -> dict[str, Any]:
    started = utc_now_iso()
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
        status = "ok"
        timed_out = False
        exit_code: int | None = cp.returncode
        stdout = cp.stdout
        stderr = cp.stderr
    except subprocess.TimeoutExpired as ex:
        status = "timeout"
        timed_out = True
        exit_code = None
        stdout = to_text(ex.stdout)
        stderr = to_text(ex.stderr)
    except FileNotFoundError as ex:
        status = "error"
        timed_out = False
        exit_code = None
        stdout = ""
        stderr = f"FileNotFoundError: {ex}"
    except Exception as ex:  # pragma: no cover - defensive path
        status = "error"
        timed_out = False
        exit_code = None
        stdout = ""
        stderr = f"{type(ex).__name__}: {ex}"

    elapsed_ms = int((time.perf_counter() - t0) * 1000)
    finished = utc_now_iso()

    return {
        "status": status,
        "timed_out": timed_out,
        "exit_code": exit_code,
        "command": command,
        "cwd": cwd,
        "started_at": started,
        "finished_at": finished,
        "duration_ms": elapsed_ms,
        "stdout": stdout,
        "stderr": stderr,
    }


def build_native_cmd(binary: pathlib.Path, binary_args: list[str]) -> list[str]:
    return [str(binary)] + binary_args


def map_host_path_to_container(
    value: str, host_mount: pathlib.Path, guest_mount: str
) -> str:
    p = pathlib.Path(value)
    if not p.is_absolute():
        return value

    try:
        rel = p.resolve().relative_to(host_mount)
    except Exception:
        return value

    return str(pathlib.PurePosixPath(guest_mount) / pathlib.PurePosixPath(*rel.parts))


def build_native_container_cmd(
    native_cmd: list[str],
    *,
    image: str,
    platform_name: str,
    host_mount: pathlib.Path,
    guest_mount: str,
    env_overrides: dict[str, str],
) -> list[str]:
    mapped = [
        map_host_path_to_container(x, host_mount, guest_mount) for x in native_cmd
    ]

    cmd: list[str] = ["docker", "run", "--rm"]
    if platform_name:
        cmd += ["--platform", platform_name]

    cmd += ["-v", f"{host_mount}:{guest_mount}", "-w", guest_mount]

    for k, v in env_overrides.items():
        cmd += ["-e", f"{k}={v}"]

    cmd += [image]
    cmd += mapped
    return cmd


def build_emu_cmd(
    analyzer: pathlib.Path,
    root: pathlib.Path,
    binary: pathlib.Path,
    binary_args: list[str],
) -> list[str]:
    return [str(analyzer), "--root", str(root), str(binary)] + binary_args


def write_text(path: pathlib.Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data, encoding="utf-8")


def write_json(path: pathlib.Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._")
    return cleaned or "unnamed"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run one test binary natively and/or under linux-analyzer"
    )
    parser.add_argument(
        "--mode",
        choices=["native", "emu", "both"],
        default="both",
        help="Execution mode",
    )
    parser.add_argument("--binary", required=True, help="Path to test binary")
    parser.add_argument(
        "--root", default=str(REPO_ROOT), help="Emulation root for --mode emu/both"
    )
    parser.add_argument(
        "--analyzer",
        default=str(DEFAULT_ANALYZER),
        help="Path to linux-analyzer executable",
    )
    parser.add_argument(
        "--timeout", type=float, default=120.0, help="Per-run timeout in seconds"
    )
    parser.add_argument(
        "--seed", type=int, help="Deterministic seed to record in artifacts"
    )
    parser.add_argument(
        "--test-name", help="Logical test name (defaults to binary stem)"
    )
    parser.add_argument("--cwd", help="Working directory for child runs")
    parser.add_argument(
        "--native-env",
        action="append",
        default=[],
        help="Native env override KEY=VALUE (repeatable)",
    )
    parser.add_argument(
        "--native-container-image",
        help="If set, run native oracle in docker image (Linux host oracle)",
    )
    parser.add_argument(
        "--native-container-platform",
        default="linux/amd64",
        help="Container platform for native oracle when --native-container-image is set",
    )
    parser.add_argument(
        "--native-container-mount-host",
        default=str(REPO_ROOT),
        help="Host path mounted into native oracle container",
    )
    parser.add_argument(
        "--native-container-mount-guest",
        default="/work",
        help="Container mount path for host repository",
    )
    parser.add_argument(
        "--emu-env",
        action="append",
        default=[],
        help="Emu env override KEY=VALUE (repeatable)",
    )
    parser.add_argument(
        "--artifacts-dir",
        default=str(DEFAULT_ARTIFACTS),
        help="Artifact base directory",
    )
    parser.add_argument(
        "--output",
        help="Output path for result.json (defaults to <artifacts-dir>/result.json)",
    )
    parser.add_argument(
        "--compare", action="store_true", help="Run compare_results.py after both runs"
    )
    parser.add_argument(
        "--compare-script",
        default=str(DEFAULT_COMPARE_SCRIPT),
        help="Path to compare_results.py",
    )
    parser.add_argument(
        "--compare-path-map",
        action="append",
        default=[],
        help="Pass-through path normalization OLD=NEW for compare_results.py (repeatable)",
    )
    parser.add_argument(
        "--compare-ignore-line-regex",
        action="append",
        default=[],
        help="Pass-through ignore-line regex for compare_results.py (repeatable)",
    )
    parser.add_argument(
        "--no-default-compare-noise-filter",
        action="store_true",
        help="Disable default emulator noise filtering regexes used during compare",
    )
    parser.add_argument(
        "binary_args", nargs=argparse.REMAINDER, help="Arguments for test binary"
    )

    args = parser.parse_args()

    binary = pathlib.Path(args.binary).resolve()
    analyzer = pathlib.Path(args.analyzer).resolve()
    root = pathlib.Path(args.root).resolve()
    native_container_mount_host = pathlib.Path(
        args.native_container_mount_host
    ).resolve()
    artifacts_dir = pathlib.Path(args.artifacts_dir).resolve()
    output_path = (
        pathlib.Path(args.output).resolve()
        if args.output
        else (artifacts_dir / "result.json")
    )

    binary_args = list(args.binary_args)
    if binary_args and binary_args[0] == "--":
        binary_args = binary_args[1:]

    test_name = args.test_name or binary.stem
    run_id = (
        f"{test_name}-{dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    )
    seed = args.seed if args.seed is not None else int(time.time())

    ensure_artifact_layout(artifacts_dir)

    native_env_overrides = parse_env_overrides(args.native_env)
    emu_env_overrides = parse_env_overrides(args.emu_env)

    base_env = os.environ.copy()

    result: dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_id,
        "created_at": utc_now_iso(),
        "test_name": test_name,
        "seed": seed,
        "binary": str(binary),
        "binary_args": binary_args,
        "root": str(root),
        "mode": args.mode,
        "native": None,
        "emu": None,
        "repro": {},
    }

    if args.mode in ("native", "both"):
        native_cmd = build_native_cmd(binary, binary_args)

        if args.native_container_image:
            native_cmd = build_native_container_cmd(
                native_cmd,
                image=args.native_container_image,
                platform_name=args.native_container_platform,
                host_mount=native_container_mount_host,
                guest_mount=args.native_container_mount_guest,
                env_overrides=native_env_overrides,
            )

        result["repro"]["native"] = {
            "command": native_cmd,
            "cwd": args.cwd,
            "env_overrides": native_env_overrides,
            "container_image": args.native_container_image,
            "container_platform": args.native_container_platform,
            "container_mount_host": str(native_container_mount_host),
            "container_mount_guest": args.native_container_mount_guest,
        }
        native_env = base_env.copy()
        if not args.native_container_image:
            native_env.update(native_env_overrides)
        result["native"] = run_command(
            native_cmd, cwd=args.cwd, env=native_env, timeout_sec=args.timeout
        )

    if args.mode in ("emu", "both"):
        emu_cmd = build_emu_cmd(analyzer, root, binary, binary_args)
        result["repro"]["emu"] = {
            "command": emu_cmd,
            "cwd": args.cwd,
            "env_overrides": emu_env_overrides,
        }
        emu_env = base_env.copy()
        emu_env.update(emu_env_overrides)
        result["emu"] = run_command(
            emu_cmd, cwd=args.cwd, env=emu_env, timeout_sec=args.timeout
        )

    write_json(output_path, result)

    # Persist canonical artifacts for replay and triage.
    write_text(artifacts_dir / "seed" / f"{run_id}.seed", f"{seed}\n")
    write_text(artifacts_dir / "binary" / f"{run_id}.txt", f"{binary}\n")
    write_text(artifacts_dir / "root" / f"{run_id}.txt", f"{root}\n")
    if result.get("native"):
        write_text(
            artifacts_dir / "trace" / f"{run_id}.native.stdout.txt",
            result["native"].get("stdout", ""),
        )
        write_text(
            artifacts_dir / "trace" / f"{run_id}.native.stderr.txt",
            result["native"].get("stderr", ""),
        )
    if result.get("emu"):
        write_text(
            artifacts_dir / "trace" / f"{run_id}.emu.stdout.txt",
            result["emu"].get("stdout", ""),
        )
        write_text(
            artifacts_dir / "trace" / f"{run_id}.emu.stderr.txt",
            result["emu"].get("stderr", ""),
        )

    compare_rc = 0
    compare_output = artifacts_dir / f"{run_id}.compare.json"
    compare_summary: dict[str, Any] | None = None
    if args.compare and args.mode == "both":
        compare_path_maps = list(args.compare_path_map)
        compare_ignore_regexes = list(args.compare_ignore_line_regex)

        if not args.no_default_compare_noise_filter:
            compare_ignore_regexes.extend(
                [
                    r"^\[INFO\].*$",
                    r"^\[WARN\].*$",
                    r"^\[ERROR\].*$",
                    r"^--- Emulation finished ---$",
                    r"^Exit status: .*$",
                    r"^Instructions executed: .*$",
                ]
            )

        compare_cmd = [
            sys.executable,
            str(pathlib.Path(args.compare_script).resolve()),
            "--result-json",
            str(output_path),
            "--output",
            str(compare_output),
        ]

        for item in compare_path_maps:
            compare_cmd += ["--path-map", item]

        for item in compare_ignore_regexes:
            compare_cmd += ["--ignore-line-regex", item]

        cp = subprocess.run(compare_cmd, capture_output=True, text=True, check=False)
        compare_rc = cp.returncode
        if cp.stdout:
            sys.stdout.write(cp.stdout)
        if cp.stderr:
            sys.stderr.write(cp.stderr)

        if compare_output.exists():
            try:
                compare_summary = json.loads(compare_output.read_text(encoding="utf-8"))
            except Exception as ex:  # pragma: no cover - defensive path
                compare_summary = {
                    "match": False,
                    "parse_error": f"{type(ex).__name__}: {ex}",
                }

    result["compare"] = {
        "enabled": bool(args.compare and args.mode == "both"),
        "return_code": compare_rc,
        "summary": compare_summary,
        "compare_json": str(compare_output) if compare_output.exists() else None,
    }

    mismatch_info: dict[str, Any] | None = None
    if (
        args.compare
        and args.mode == "both"
        and compare_rc != 0
        and compare_summary is not None
    ):
        failure_root = artifacts_dir / "failures" / safe_name(test_name)
        first_mismatch = failure_root / "first_mismatch"
        created = False

        if not first_mismatch.exists():
            created = True
            first_mismatch.mkdir(parents=True, exist_ok=True)

            write_text(first_mismatch / "run_id.txt", f"{run_id}\n")
            write_text(first_mismatch / "seed.txt", f"{seed}\n")
            write_text(first_mismatch / "binary.txt", f"{binary}\n")
            write_text(first_mismatch / "root.txt", f"{root}\n")

            trace_dir = artifacts_dir / "trace"
            for suffix in (
                "native.stdout.txt",
                "native.stderr.txt",
                "emu.stdout.txt",
                "emu.stderr.txt",
            ):
                src = trace_dir / f"{run_id}.{suffix}"
                if src.exists():
                    shutil.copy2(src, first_mismatch / src.name)

        mismatch_info = {
            "path": str(first_mismatch),
            "created": created,
        }

    result["first_mismatch_artifact"] = mismatch_info

    # Rewrite result.json with compare summary and mismatch metadata.
    write_json(output_path, result)

    if mismatch_info and mismatch_info.get("created"):
        first_mismatch = pathlib.Path(mismatch_info["path"])
        shutil.copy2(output_path, first_mismatch / "result.json")
        if compare_output.exists():
            shutil.copy2(compare_output, first_mismatch / "compare.json")

    # Non-zero on infrastructure issues, and on compare mismatch when --compare is used.
    infra_failure = False
    for mode_name in ("native", "emu"):
        entry = result.get(mode_name)
        if entry and entry.get("status") != "ok":
            infra_failure = True

    pass_status = not infra_failure and (not args.compare or compare_rc == 0)

    # Machine-readable summary to stdout for CI/log scraping.
    print(
        f"TEST:{test_name}:PASS:run_id={run_id}"
        if pass_status
        else f"TEST:{test_name}:FAIL:run_id={run_id}"
    )
    print(
        json.dumps({"run_id": run_id, "result_json": str(output_path)}, sort_keys=True)
    )

    if infra_failure:
        return 2

    if args.compare and compare_rc != 0:
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
