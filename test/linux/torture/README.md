# Linux Torture Suite

This directory contains aggressive validation infrastructure for the Linux ELF x86-64 emulator.

## Purpose

The torture suite focuses on failure boundaries and semantic fidelity:

- adversarial ELF loader coverage (malformed, near-valid, and edge-valid inputs)
- syscall ABI/behavior correctness under hostile and race-prone conditions
- differential checks against native Linux behavior
- reproducible fuzzing, soak, and regression triage workflows

## Test Taxonomy

- `loader.adversarial`: malformed/truncated/alignment/overlap/RELRO/NX/interpreter resolution
- `syscall.semantics`: fd/openat/pipe/getdents/mmap/brk/futex/signal/poll/select/epoll/socket/procfs/time/scheduler
- `differential`: native-vs-emulated output/status/trace comparisons
- `fuzz.elf`: mutational corpus-based ELF fuzzing
- `fuzz.stateful`: constrained syscall state-machine fuzzing
- `soak.stability`: long-run mixed-workload deadlock/leak/perf checks

## Directory Layout

`fixtures/`
- `elf/valid/`: expected to run
- `elf/near-valid/`: expected deterministic reject or run depending manifest
- `elf/malformed/`: expected clean reject
- `manifest.json`: expected outcome and rationale for each fixture

`tools/`
- `run_native_vs_emu.py`: single-test runner (`native`, `emu`, or `both`)
- `compare_results.py`: deterministic comparator with normalization hooks
- `replay_failure.sh`: one-command repro from artifact metadata
- `capture_syscall_trace.py`: deterministic trace capture helper (native `strace` where available, emulator verbose fallback)
- `run_mustpass.py`: executes `baseline.mustpass.json` and emits CI-friendly `TEST:<name>:PASS|FAIL:<details>` lines
- `run_differential_suite.py`: runs deterministic case matrix (`differential.deterministic.json`) against native Linux oracle + emulator

`baseline.mustpass.json`
- initial merge-gate candidate list for fast Phase 10 must-pass checks

`differential.deterministic.json`
- deterministic native-vs-emulated case matrix used to satisfy sprint differential-count criteria

`artifacts/`
- `seed/`: deterministic seed records
- `binary/`: tested binary metadata
- `root/`: emulation root metadata
- `trace/`: captured stdout/stderr traces
- `result.json`: latest machine-readable run summary
- `failures/<test>/first_mismatch/`: sticky first mismatch artifact for deterministic replay and triage

## Fixture and Naming Conventions

- Prefix fixture files by class: `elf_`, `sys_`, `sig_`, `fx_`
- Keep one deterministic purpose per fixture
- Every fixture must have an expected outcome in `fixtures/manifest.json`
- Any randomized fixture must accept and report a deterministic seed

## Deterministic Seed Policy

- All randomized tests must accept `--seed` and record it in artifacts
- If no seed is provided, the runner emits one derived from current time and records it
- A failing artifact must be replayable by seed + binary + root + args only

## Failure Triage Labels

Use one primary label when logging failures:

- `loader-parse`
- `loader-map`
- `relocation`
- `dynlink-search`
- `vm-permissions`
- `syscall-fd`
- `syscall-memory`
- `syscall-signal`
- `syscall-futex-scheduler`
- `syscall-io-readiness`
- `procfs-vdso`
- `harness-infra`

## Typical Workflow

Run a deterministic differential check:

```bash
python3 test/linux/torture/tools/run_native_vs_emu.py \
  --mode both \
  --binary /path/to/test_binary \
  --root /path/to/sysroot \
  --seed 12345 \
  --compare
```

Compare previously captured outputs:

```bash
python3 test/linux/torture/tools/compare_results.py \
  --result-json test/linux/torture/artifacts/result.json
```

Replay latest failure artifact:

```bash
test/linux/torture/tools/replay_failure.sh test/linux/torture/artifacts/result.json emu
```

Run the must-pass baseline slice:

```bash
python3 test/linux/torture/tools/run_mustpass.py \
  --baseline test/linux/torture/baseline.mustpass.json \
  --root /path/to/emulation-root
```

Run the deterministic differential suite (Docker native oracle):

```bash
python3 test/linux/torture/tools/run_differential_suite.py \
  --cases test/linux/torture/differential.deterministic.json \
  --root /path/to/emulation-root \
  --native-container-image debian:bookworm-slim
```
