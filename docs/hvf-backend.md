# The Hypervisor.framework backend

`hvf` runs arm64 guest code natively on Apple silicon instead of interpreting it. It is selected with
`--backend=hvf` and is built only on arm64 macOS; on every other platform the target is absent and the
backend name is rejected at parse time.

The reason to use it is not raw speed alone — it is **Pointer Authentication**. PAC on this backend is the
CPU's own implementation using Apple's key schedule, so a `pacia`/`autia` pair produces exactly the values
the guest's own libraries produce. On an interpreted backend PAC is a model, and a model that disagrees with
the silicon in any bit is a model that eventually traps inside `libobjc`.

## Requirements

- Apple silicon (arm64) running macOS.
- The `com.apple.security.hypervisor` entitlement **on the executable that calls the framework**, not on the
  backend dylib. The build signs the test and analyzer binaries ad-hoc with that entitlement; a paid
  developer account is not needed for local use.
- A host that is not itself a VM. `sysctl kern.hv_support` must read `1`.

GitHub's macOS runners are virtual machines, so `kern.hv_support` reads `0` there and every HVF test skips.
CI prints that sysctl and the binary's entitlements before running the backend tests, so a skip is
distinguishable from a regression at a glance rather than by re-running the job locally.

## Capabilities

The table is what the backend reports through the emulator interface. Callers branch on these rather than on
the backend's name.

| Capability | `unicorn` | `hvf` | Why |
|---|---|---|---|
| `supports_instruction_counting()` | `true` | `false` | Hypervisor.framework exposes no retired-instruction count for a guest. See the note below — this does *not* mean `start(n)` is inexact. |
| `supports_global_memory_execution_hooks()` | `true` | `false` | Guest code runs on real cores; there is no interception point between instructions. Address-specific execution hooks still work, implemented by patching a `brk`. |
| `supports_multiple_vcpus()` | `false` | `false` | The framework allows 64, but each vCPU costs a dedicated thread and sogen has no scheduler that wants more than one yet. |
| `is_stop_thread_safe()` | `false` | `true` | `hv_vcpus_exit()` is the one call that is legal from a thread other than the vCPU's owner. |
| Pointer Authentication | modelled | **native** | The decisive difference. Apple's own keys and algorithm. |
| `hook_instruction(svc)` | yes | yes | `svc` traps to the EL1 stub, which `hvc`s out; the backend reads the immediate from `ESR_EL1.ISS[15:0]`. |
| MMIO | yes | yes, with a caveat | Loads and stores that set `ESR_EL2.ISV` decode from the syndrome. `stp`/`ldp`/SIMD/writeback forms report `ISV=0` and carry no syndrome. |
| Memory permissions | 4 KiB | 4 KiB | Stage-2 pages are 16 KiB, but permissions live in the stage-1 tables the backend builds, so guest granularity is unaffected. |
| Save/restore registers | yes | yes | Includes all ten PAC key registers, or a restored snapshot would fail every `autia`. |

### `supports_instruction_counting() == false` does not mean `start(n)` is approximate

These are two different questions and it is worth being precise about which one the flag answers.

`start(n)` with a non-zero `n` is **exact** on this backend. It single-steps via `MDSCR_EL1.SS`, so after
`start(2)` the guest has retired exactly two instructions and `pc` has advanced by exactly eight bytes. The
conformance suite asserts this against both engines from the same test body.

What the backend cannot do is report a count while running *freely*. Single-stepping costs a VM exit per
instruction — measured at roughly 5,000x the cost of free-running execution — so it is a debugging tool, not
a way to instrument a whole process. `supports_instruction_counting()` is therefore consumed by callers that
want to slice time by instruction count during unbounded execution (`windows_emulator`'s
`use_instruction_precision`), and answering `false` turns that off in favour of a scheme that does not need
a per-instruction count.

## Deviations from the Unicorn backend

Things that are deliberately different, so that a reader who finds them surprising does not "fix" them.

1. **The VM is a process-wide singleton.** A second `hv_vm_create()` returns `HV_BUSY` and
   `hv_vm_destroy()` tears the address space down for everyone. The backend refcounts a shared
   `vm_reference` instead of giving each emulator its own VM.

2. **Every `hv_vcpu_*` call is marshalled to the owning thread.** The vCPU is bound to the thread that
   created it, so reading a register from the scheduler thread posts a command to the vCPU's thread and
   waits. `stop()` is the exception and is genuinely lock-free, because `hv_vcpus_exit()` may be called from
   anywhere.

3. **Guest code runs at EL0 under a small EL1 stub the backend builds.** Hypervisor.framework on arm64
   exposes no `HV_SYS_REG_HCR_EL2`, so the backend cannot ask the CPU to route EL0 exceptions straight to
   EL2. The stub is a vector table that catches the EL0 exception and issues `hvc` to reach the VMM. This is
   why a guest `svc` arrives with `ESR_EL2.EC == 0x16` (HVC64, from the stub) and the real cause has to be
   read out of `ESR_EL1` (`EC == 0x15`, SVC64).

4. **`exit.exception.syndrome` is only meaningful for `HV_EXIT_REASON_EXCEPTION`.** For any other exit
   reason the field holds stale data from the previous exception. The dispatcher checks the reason first.

5. **`svc` is a trap and `brk` is fault-like.** After an `svc` exit, `ELR_EL1` already points at the
   following instruction. After a `brk` it points at the `brk` itself, so a patched execution hook has to
   restore the original instruction and re-enter rather than advance `pc`.

## Tests

`arm64_conformance_test.cpp` is the backend-neutral suite: one body of tests, parameterized over every
engine, so `unicorn` and `hvf` are held to the same behaviour by construction. Where an engine genuinely
cannot do something it says so through a capability method and the test skips with that reason — there is no
list of backend names in the test file. On a machine without HVF the whole `hvf` parameterization skips at
`SetUp()` with the framework's own error message.

`arm64_backend_test.cpp` keeps the tests that are specific to one engine: Unicorn's interpreter-only
behaviours, and HVF's framework-level ones (VM refcounting, thread affinity, stage-2 aborts, watchpoints).
