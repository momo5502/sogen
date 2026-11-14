# Seccomp backend

The seccomp backend executes ARM64 Windows code directly on ARM64 Linux and Android hosts.
It uses Sogen's Windows PE loader, process and thread initialization, TEB/PEB setup, and syscall dispatcher.
Select it with `--backend seccomp` or `EMULATOR_SECCOMP=1`.

Counter-only native timing is enabled by default.
The analyzer keeps shared-data MMIO reads intercepted, including with `--native-timing`, and refreshes
the Windows timestamps on each read. The flag takes no argument; the seccomp backend always uses native
counter reads. Native timing is incompatible with `--reproducible`.

## Execution and exceptions

Guest addresses are host addresses in a reserved `[64 KiB, 64 GiB)` window. Reservation fails if
host mappings overlap the window. A thread-local seccomp filter traps syscall instructions in this
window with `SIGSYS`. The backend decodes the Windows syscall number from the `SVC` immediate and
passes it to the shared syscall dispatcher, with PC pointing to the `SVC` instruction.

Guest entry uses `SIGUSR2` and signal return; `SIGUSR1` requests a scheduler stop from another host
thread. The backend owns these signals and its synchronous fault handlers while the emulator is alive.
Signal entry restores the host's X18 and TLS before calling C++. Guest GPRs, NZCV, SP, PC and FPSIMD
state are saved before returning to the host stack, where emulator callbacks execute.

`SIGILL` becomes an ARM64 undefined-instruction exception at the faulting PC. The Windows exception
dispatcher delivers `STATUS_ILLEGAL_INSTRUCTION` with the saved guest registers. This includes
`MRS CurrentEL` at EL0; a guest handler can advance its saved PC and resume execution.

`PSTATE.SS` uses Linux `PTRACE_SINGLESTEP` to execute one instruction. A child tracer saves the host
registers, FPSIMD state and TLS, installs the guest state, captures the result, and restores the host
state before detaching. Successful steps report a software-step exception at the next PC with SS
cleared. Instructions that raise another exception, including `SVC`, cancel the pending step.
Tracing failures produce an error, including host policies that disallow a same-UID child from tracing
the CPU thread.

## MMIO

Read-only MMIO regions have inaccessible host pages. Memory faults return execution to the host
stack, where the backend decodes the load, calls the region-relative read callback, updates the guest
registers, and advances PC. Supported forms are:

- Integer and SIMD loads with unsigned, unscaled, register, pre-indexed or post-indexed offsets.
- Scalar `LDAR`, `LDAPR` and `LDAPUR` loads.
- Single-vector `LD1` with a `#16` post-increment and `.16B`, `.8H`, `.4S` or `.2D` arrangement.
- Byte-lane-zero `LD2` and `LD4` with `#2` and `#4` post-increments.

Indexed loads write back the base register, including SP, after a successful read. Byte-lane loads
preserve the other vector bytes. Emulated loads complete a pending software step.
Unsupported accesses raise an error, including stores, paired or exclusive loads, and overlapping
integer destination/base writeback.

## Host requirements and limits

The backend supports one emulator instance per process, one vCPU on its original host thread, and
4 KiB host pages. The seccomp filter and no-new-privileges setting remain active on that thread after
the emulator is destroyed. Guest code shares the host address space; this backend does not isolate
untrusted programs from the host.

Snapshots, counter interception, instruction-count execution budgets, execution hooks, recoverable
memory faults, and enabled hardware debug registers are unsupported and raise an error. Disabled debug
registers read as zero and accept zero. Thread stacks are committed in full because stack growth
cannot recover from memory faults. Memory read/write and basic-block observation hooks are accepted
without producing callbacks. Unsupported guest signals stop execution with PC and signal diagnostics.

## Build and run

The backend is enabled by default on ARM64 Linux and Android. Configure with
`-DSOGEN_ENABLE_SECCOMP=OFF` to disable it. Its unit tests are included in `windows-emulator-test`
and require no Windows binaries or cross compiler.

```sh
cmake --preset release
cmake --build --preset release --target analyzer windows-emulator-test
build/release/artifacts/windows-emulator-test --gtest_filter='*Seccomp*'
```

Use a Windows ARM64 emulation root and an ARM64 Windows executable. The root must contain
`filesys/c/windows/system32/apisetschema.dll` and registry hives. Programs can be provided through
the root filesystem or a host mount:

```sh
build/release/artifacts/analyzer --backend seccomp --no-inst-precision \
  --emulation /path/to/windows-arm64-root \
  --mount '/path/to/test-sample.exe:C:\test-sample.exe:ro' \
  -s 'C:\test-sample.exe'
```

The backend tests cover illegal-instruction delivery, hardware single-stepping, MMIO load semantics,
register preservation and base-register writeback. The existing ARM64 integration tests use
`EMULATOR_ARM64_ROOT`; select this backend with `EMULATOR_SECCOMP=1`. The ordinary `test-sample`
exercises Windows startup, console output, guest exception handling and single-stepping.
