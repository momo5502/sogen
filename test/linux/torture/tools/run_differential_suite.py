#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_CASES = (
    REPO_ROOT / "test" / "linux" / "torture" / "differential.deterministic.json"
)
DEFAULT_RUNNER = pathlib.Path(__file__).resolve().parent / "run_native_vs_emu.py"
DEFAULT_ANALYZER = REPO_ROOT / "build" / "debug" / "artifacts" / "linux-analyzer"
DEFAULT_ARTIFACTS = REPO_ROOT / "test" / "linux" / "torture" / "artifacts"

DEFAULT_COMPARE_IGNORE_REGEXES = [
    r"^$",
    r"^release: .*$",
    r"^PATH=.*$",
    r"^  PATH=.*$",
    r"^  HOSTNAME=.*$",
    r"^  TERM=.*$",
    r"^snprintf works: .*$",
    r"^Unable to find image '.*' locally$",
    r"^[A-Za-z0-9._/-]+: Pulling from .*$",
    r"^[0-9a-f]{12}: Pulling fs layer$",
    r"^[0-9a-f]{12}: Download complete$",
    r"^[0-9a-f]{12}: Pull complete$",
    r"^Digest: sha256:.*$",
    r"^Status: Downloaded newer image for .*$",
]


def load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def resolve_path(base: pathlib.Path, raw: str) -> pathlib.Path:
    p = pathlib.Path(raw)
    if p.is_absolute():
        return p
    return (base / p).resolve()


def case_expected(case: dict[str, Any]) -> str:
    value = str(case.get("expected", "match")).strip().lower()
    if value not in {"match", "mismatch"}:
        raise ValueError(
            f"Invalid expected value '{value}' in case '{case.get('name')}'"
        )
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run deterministic native-vs-emulated differential suite"
    )
    parser.add_argument(
        "--cases",
        default=str(DEFAULT_CASES),
        help="Path to deterministic case list JSON",
    )
    parser.add_argument(
        "--runner", default=str(DEFAULT_RUNNER), help="Path to run_native_vs_emu.py"
    )
    parser.add_argument(
        "--analyzer", default=str(DEFAULT_ANALYZER), help="Path to linux-analyzer"
    )
    parser.add_argument(
        "--root", default=str(REPO_ROOT), help="Emulation root (host path)"
    )
    parser.add_argument(
        "--artifacts-dir", default=str(DEFAULT_ARTIFACTS), help="Artifacts root"
    )
    parser.add_argument(
        "--native-container-image",
        default="debian:bookworm-slim",
        help="Docker image used for native Linux oracle",
    )
    parser.add_argument(
        "--native-container-platform",
        default="linux/amd64",
        help="Docker platform used for native Linux oracle",
    )
    parser.add_argument(
        "--compare-ignore-line-regex",
        action="append",
        default=[],
        help="Additional compare ignore-line regex (repeatable)",
    )
    parser.add_argument(
        "--no-default-compare-normalization",
        action="store_true",
        help="Disable default ignore regexes used to normalize expected env/version noise",
    )
    parser.add_argument(
        "--skip-docker-pull",
        action="store_true",
        help="Skip pre-pulling native oracle container image",
    )
    parser.add_argument("--timeout", type=float, help="Override timeout for all cases")
    parser.add_argument("--summary-output", help="Override summary output path")
    args = parser.parse_args()

    cases_path = pathlib.Path(args.cases).resolve()
    runner = pathlib.Path(args.runner).resolve()
    analyzer = pathlib.Path(args.analyzer).resolve()
    root = pathlib.Path(args.root).resolve()
    artifacts_dir = pathlib.Path(args.artifacts_dir).resolve()

    suite = load_json(cases_path)
    defaults = suite.get("defaults", {})
    default_timeout = float(
        args.timeout if args.timeout is not None else defaults.get("timeout", 120.0)
    )
    cases = suite.get("cases", [])

    if not isinstance(cases, list) or len(cases) == 0:
        print("error: no cases defined", file=sys.stderr)
        return 2

    differential_dir = artifacts_dir / "differential"
    differential_dir.mkdir(parents=True, exist_ok=True)
    summary_path = (
        pathlib.Path(args.summary_output).resolve()
        if args.summary_output
        else (differential_dir / "summary.json")
    )

    summary: dict[str, Any] = {
        "schema_version": 1,
        "cases_file": str(cases_path),
        "analyzer": str(analyzer),
        "root": str(root),
        "native_container_image": args.native_container_image,
        "native_container_platform": args.native_container_platform,
        "total_cases": len(cases),
        "passed": 0,
        "expected_mismatches": 0,
        "unexpected_failures": 0,
        "results": [],
    }

    if not args.skip_docker_pull:
        pull = subprocess.run(
            [
                "docker",
                "pull",
                "--platform",
                args.native_container_platform,
                args.native_container_image,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if pull.returncode != 0:
            if pull.stdout:
                sys.stdout.write(pull.stdout)
            if pull.stderr:
                sys.stderr.write(pull.stderr)
            print(
                f"error: failed to pull docker image {args.native_container_image}",
                file=sys.stderr,
            )
            return 2

    for case in cases:
        name = str(case["name"])
        binary = resolve_path(REPO_ROOT, str(case["binary"]))
        binary_args = [str(x) for x in case.get("args", [])]
        expected = case_expected(case)
        timeout = float(case.get("timeout", default_timeout))

        result_json = differential_dir / f"{name}.result.json"

        cmd = [
            sys.executable,
            str(runner),
            "--mode",
            "both",
            "--compare",
            "--binary",
            str(binary),
            "--analyzer",
            str(analyzer),
            "--root",
            str(root),
            "--native-container-image",
            args.native_container_image,
            "--native-container-platform",
            args.native_container_platform,
            "--native-container-mount-host",
            str(root),
            "--native-container-mount-guest",
            "/work",
            "--compare-path-map",
            f"{root}=<ROOT>",
            "--compare-path-map",
            "/work=<ROOT>",
            "--timeout",
            str(timeout),
            "--test-name",
            name,
            "--artifacts-dir",
            str(artifacts_dir),
            "--output",
            str(result_json),
        ]

        compare_ignore = []
        if not args.no_default_compare_normalization:
            compare_ignore.extend(DEFAULT_COMPARE_IGNORE_REGEXES)
        compare_ignore.extend(args.compare_ignore_line_regex)
        compare_ignore.extend(
            [str(x) for x in case.get("compare_ignore_line_regex", [])]
        )

        for item in compare_ignore:
            cmd += ["--compare-ignore-line-regex", item]

        if binary_args:
            cmd.append("--")
            cmd.extend(binary_args)

        cp = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if cp.stdout:
            sys.stdout.write(cp.stdout)
        if cp.stderr:
            sys.stderr.write(cp.stderr)

        compare_match = False
        compare_rc = None
        first_mismatch = None
        failure_reason = ""

        if not result_json.exists():
            failure_reason = "missing result.json"
        else:
            result = load_json(result_json)
            compare = result.get("compare") or {}
            compare_rc = compare.get("return_code")
            compare_summary = compare.get("summary") or {}
            compare_match = bool(compare_summary.get("match", False))
            first_mismatch = result.get("first_mismatch_artifact")

        case_pass = (expected == "match" and compare_match) or (
            expected == "mismatch" and not compare_match
        )

        if case_pass:
            summary["passed"] += 1
            if expected == "mismatch":
                summary["expected_mismatches"] += 1
                print(f"TEST:{name}:PASS:expected-mismatch")
            else:
                print(f"TEST:{name}:PASS:match")
        else:
            summary["unexpected_failures"] += 1
            if not failure_reason:
                if expected == "match":
                    failure_reason = f"unexpected mismatch (compare_rc={compare_rc})"
                else:
                    failure_reason = "expected mismatch but outputs matched"
            print(f"TEST:{name}:FAIL:{failure_reason}")

        summary["results"].append(
            {
                "name": name,
                "binary": str(binary),
                "args": binary_args,
                "expected": expected,
                "compare_match": compare_match,
                "compare_rc": compare_rc,
                "first_mismatch_artifact": first_mismatch,
                "result_json": str(result_json),
                "runner_return_code": cp.returncode,
            }
        )

    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    print(
        json.dumps(
            {
                "summary_json": str(summary_path),
                "total": summary["total_cases"],
                "unexpected_failures": summary["unexpected_failures"],
            },
            sort_keys=True,
        )
    )

    return 1 if summary["unexpected_failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
