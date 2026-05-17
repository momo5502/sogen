#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  replay_failure.sh [result-json-or-artifact-dir] [native|emu|both]

Examples:
  replay_failure.sh test/linux/torture/artifacts/result.json emu
  replay_failure.sh test/linux/torture/artifacts both
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

ARTIFACT_INPUT="${1:-test/linux/torture/artifacts/result.json}"
MODE="${2:-emu}"

if [[ -d "$ARTIFACT_INPUT" ]]; then
  RESULT_JSON="$ARTIFACT_INPUT/result.json"
else
  RESULT_JSON="$ARTIFACT_INPUT"
fi

if [[ ! -f "$RESULT_JSON" ]]; then
  echo "error: result json not found: $RESULT_JSON" >&2
  exit 2
fi

python3 - "$RESULT_JSON" "$MODE" <<'PY'
import json
import os
import pathlib
import shlex
import subprocess
import sys


result_path = pathlib.Path(sys.argv[1])
mode = sys.argv[2]

if mode not in {"native", "emu", "both"}:
    print(f"error: invalid mode '{mode}', expected native|emu|both", file=sys.stderr)
    sys.exit(2)

data = json.loads(result_path.read_text(encoding="utf-8"))
repro = data.get("repro", {})

modes = ["native", "emu"] if mode == "both" else [mode]

overall_rc = 0
for m in modes:
    entry = repro.get(m, {})
    cmd = entry.get("command") or data.get(m, {}).get("command")
    if not cmd:
        print(f"[replay] no command recorded for mode '{m}', skipping", file=sys.stderr)
        overall_rc = max(overall_rc, 2)
        continue

    cwd = entry.get("cwd")
    env_overrides = entry.get("env_overrides") or {}
    env = os.environ.copy()
    env.update({str(k): str(v) for k, v in env_overrides.items()})

    print(f"[replay] mode={m}")
    print(f"[replay] cmd={shlex.join(cmd)}")
    if cwd:
        print(f"[replay] cwd={cwd}")

    rc = subprocess.call(cmd, cwd=cwd, env=env)
    print(f"[replay] exit={rc}")
    overall_rc = max(overall_rc, rc if isinstance(rc, int) else 1)

sys.exit(overall_rc)
PY
