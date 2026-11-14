# Native ARM64 proof of concept

Enable with `-DSOGEN_ENABLE_NATIVE_ARM64=ON` on an ARM64 Linux or Android host.
Select with `--backend native-arm64` or `EMULATOR_NATIVE_ARM64=1`.
Use `--native-timing --no-inst-precision`; deterministic counter interception and MMIO are not implemented.

This is a normal `arm64_emulator` backend. The existing Windows PE loader, process context,
`emulator_thread`, TEB/PEB initialization, ntdll bootstrap and syscall dispatcher are shared with
the other backends. The backend never constructs a separate Windows environment or handles
`NtWriteFile` itself.

Guest addresses are host addresses in a reserved `[64 KiB, 64 GiB)` window. The window is claimed
without replacing existing mappings. A thread-local seccomp filter traps syscalls from below
64 GiB with SIGSYS, leaving host syscalls above it alone. The backend decodes the Windows service
number from the SVC immediate, presents PC at the SVC to the existing hook, and applies the hook's
continuation semantics. The host signal wrapper restores Android's x18 and TLS before entering
C++; guest x18 remains in the kernel's signal frame. GPRs, NZCV, SP, PC and FPSIMD state are copied
between the backend and signal frames. Callbacks run on the host stack after leaving the handler.

The backend owns SIGUSR1/SIGUSR2 and synchronous fault handlers while alive. SIGUSR2 enters the
guest through signal return; SIGUSR1 lets the shared scheduler stop it from another host thread.
Only one machine, one vCPU, its original host thread, and 4 KiB host pages are supported.
The seccomp filter and no-new-privileges setting are irreversible for that host thread.
This shares the host address space and is not a security boundary for untrusted programs.

Unimplemented operations throw: snapshots, MMIO, counter interception, instruction limits,
execution hooks, recoverable memory faults and
enabled hardware debug registers. Disabled debug registers read as zero and accept zero.
Hardware `SIGILL` is reported through the ARM64 undefined-instruction hook at the faulting PC,
preserving the guest registers. The shared Windows exception dispatcher delivers
`STATUS_ILLEGAL_INSTRUCTION` to guest handlers. This includes `MRS CurrentEL`, which is undefined
at EL0; the Windows UCRT's handler recognizes the probe and advances its saved PC itself.
`PSTATE.SS` executes one instruction using Linux `PTRACE_SINGLESTEP` and then reports the ARM64
software-step exception with the next PC and SS cleared. A short-lived child tracer saves the
host registers, FPSIMD state and TLS, installs the guest state, captures the step or fault, and
restores the host state before detaching. This requires permission for a same-UID child to trace
the CPU thread. It fails explicitly if the kernel or host policy disallows tracing.
An instruction that raises another exception, including SVC, cancels its pending step.
Memory read/write and basic-block hooks are accepted but never fire, matching Sogen's FEX
backend. This permits the analyzer's default memory watches but produces no memory-access observations.
Other unsupported guest signals terminate the run with PC and signal diagnostics. The common Windows
setup skips optional hooks using the backend's reported capabilities and commits thread stacks
in full because restartable faults are unavailable.

`native-arm64-hello` builds `hello.c` as an ARM64 Windows PE using the MinGW toolchain in `~/mingw/bin`
(or on PATH; override with `-DNATIVE_ARM64_MINGW_CC=/path/to/aarch64-w64-mingw32-gcc`).
It links `kernel32` without the CRT, calls `GetStdHandle` and `WriteFile`, checks the result and byte count,
then calls `ExitProcess`. The Windows DLLs route the write through `NtWriteFile` using the shared Windows
process/thread setup. There are no handwritten syscall instructions or hardcoded service numbers in the sample.
No generated PE or object files belong in git.

`native-arm64-illegal-instruction` is a separate regression sample. Its Windows vectored exception
handler checks the illegal-instruction status, faulting PC and unchanged X16, advances the saved PC,
and verifies execution resumes after `MRS CurrentEL`. The backend regression also covers X0, X16,
X30 and XZR destinations and checks unchanged NZCV, SP and other general registers.
`native-arm64-single-step` checks Windows `NtContinue` with SS set, delivery of `EXCEPTION_SINGLE_STEP`
after exactly one instruction, the handler's saved PC and registers, and continued execution with SS clear.

On the phone, from `~/sogen`, after committing on the PC and pushing the branch to the phone:

```sh
cmake --preset release -DSOGEN_ENABLE_NATIVE_ARM64=ON -DSOGEN_ENABLE_AAUDIO=OFF -DSOGEN_ENABLE_CLANG_TIDY=OFF
cmake --build --preset release --target analyzer native-arm64-hello arm64-backend-test native-arm64-emulation-test -j2
EMULATOR_NATIVE_ARM64=1 build/release/artifacts/arm64-backend-test \
  --gtest_filter=Arm64BackendTest.SyscallReturnsToEl0WithArgumentsAndRegistersIntact
EMULATOR_ARM64_ROOT="$HOME/sogen/win2025root_arm64" \
  ctest --test-dir build/release -R '^native-arm64-emulation-test$' --output-on-failure
build/release/artifacts/analyzer --backend native-arm64 --native-timing --no-inst-precision \
  --emulation "$HOME/sogen/win2025root_arm64" \
  --mount "$HOME/sogen/build/release/src/samples/native-arm64-hello/native-arm64-hello.exe:C:\native-arm64-hello.exe:ro" \
  -s 'C:\native-arm64-hello.exe'
```

The end-to-end assertion checks both `STATUS_SUCCESS` and exactly `Hello World\n`, using the
real Windows process/thread initialization and console syscall path.
The root must include `filesys/c/windows/system32/apisetschema.dll` and matching registry hives;
an older root containing only `api-set.bin` does not satisfy the shared loader's requirements.
