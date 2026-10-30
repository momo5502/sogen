# Hypervisor.framework backends

macOS backends built on Apple's Hypervisor.framework (`sogen::hvf`). Which one is built depends on the host:

| Host | Library | Guest | Selection |
|---|---|---|---|
| Apple Silicon | `hvf-arm64-emulator` | ARM64 Windows programs | default for ARM64 images when the process holds the entitlement; `EMULATOR_UNICORN=1` opts out |
| Intel | `hvf-x86-64-emulator` | x64 Windows programs | `EMULATOR_HVF=1` or `--backend hvf` |

Executables that create a VM need the `com.apple.security.hypervisor` entitlement
(`sogen_enable_hypervisor_entitlement` signs them ad hoc). A process can own one VM at a time.

## ARM64 (Apple Silicon)

The Windows guest runs at EL0. The backend owns an EL1 vector page that turns every exception into a VM exit,
and its own stage-1 tables, which map each 4 KiB guest page to a guest physical page backed by host memory
(Hypervisor.framework maps host memory in 16 KiB blocks). Stage 1 carries the EL0 permissions, so every
fault reaches the host as an abort with its address.

Multiple vCPUs are supported; see `docs/multi-vcpu-design.md` §8a for the threading model, TLB coherence and
the hooked-instruction protocol. Backend contract tests: `arm64-backend-test` (the `Arm64MultiVcpuBackendTest`
suite runs two vCPUs).

## x86-64 (Intel)

**Status: compiles and links for x86_64; not yet run on Intel hardware.** The first run on an Intel Mac should
be `EMULATOR_HVF=1 x86-64-backend-test`, the backend contract test (it passes on Unicorn). A VM entry failure
reports the VMX exit qualification, which identifies the guest-state field that the processor rejected.

The guest runs at CPL3 in 64-bit mode, and VMX gives the backend controls that WHP and KVM do not expose:

- **Exceptions.** The exception bitmap intercepts every vector, so each guest exception exits at its faulting
  context. There is no guest IDT, TSS or exception stub; a WOW64 fault needs no IRETQ trampoline, because the
  cached segment state (including CS.L) is written directly.
- **Syscalls.** `EFER.SCE` stays clear, so `syscall` raises `#UD` with every register intact and the exit goes
  to the syscall hook. RIP is at the instruction during the hook, as on WHP and KVM.
- **Software interrupts.** With no IDT, `int n` faults with `#GP` while fetching its gate; the backend reads the
  opcode and reports vector `n` (`int 0x29` fast fail, `int 0x2c`, `int 0x2d`).
- **CPUID, RDTSC, RDTSCP.** CPUID always exits; RDTSC exiting is enabled only while an rdtsc/rdtscp hook exists.
  Without a skipping hook the host instruction supplies the result.
- **Memory.** The guest's page tables (outside its address space) map every page present, writable and
  user-accessible; the EPT carries the real permissions per page. An unmapped address is a guest `#PF`; an
  access the page does not allow is an EPT violation whose linear address the exit reports. Pages without any
  access are left out of the EPT.
- **Execution hooks and single steps.** A hooked page loses EPT execute. After the hooks run, the instruction
  is stepped with the monitor trap flag and the page is protected again. The same flag implements
  `start(count)`.

Known differences from WHP/KVM: guest stores to an MMIO range fault like stores to read-only memory (there is
no instruction emulator to decode them); data access hooks and basic-block hooks are not reported; one vCPU.

## x64 programs on Apple Silicon: FEX inside the VM

Hypervisor.framework on Apple Silicon executes ARM64 only, so x64 code has to be translated. The FEX backend's
hypervisor mode keeps FEX's compiler on the host and runs its dispatcher and generated code at EL0 in the VM.

**Feasibility spike.** `fex-hvf-spike-test` runs `mov rax, 1; add rax, 2; syscall` through FEX
inside the VM: the dispatcher's block-cache miss exits to the host, `ContextImpl::CompileBlock` runs there, the
compiled block runs in the VM, and the syscall exits with RAX = 3 and RIP at the instruction (100 of 100 runs).
No FEX change was needed:

- Generated code reaches host C++ through `CpuStateFrame::Pointers` and four literals at the end of the
  dispatcher (CTX, SleepThread, CompileBlock, CompileSingleStep). Each is replaced by an address the VM does not
  map, so the call arrives as an instruction abort; the host performs it with the vCPU's argument registers and
  resumes at LR.
- Guest memory and FEX's own memory (thread state, lookup caches, code buffer) are mapped into the VM at their
  host addresses, FEX's memory on first touch.
- The host never runs generated code, so FEX's executable buffers are allocated as plain data and the VM's
  stage-1 tables grant execute.

**Constraint found on the way:** `hv_vm_map` of MAP_JIT memory panics the whole machine on macOS 27 (SPTM
`VIOLATION_ILLEGAL_MAPPING_TYPE`, page type `XNU_USER_JIT`), and FEX allocates its code buffers and dispatcher
with MAP_JIT on Apple. The ARM64 backend therefore refuses any 16 KiB host page that contains executable memory
(`is_plain_data`), and `HostMemoryBacksGuestPagesOnlyWhenItIsNotExecutable` covers it.

**Isolation.** The VM maps only guest memory and the FEX memory its generated code touches, so a guest can no
longer reach the rest of sogen's memory, as it can with in-process FEX. It can still reach FEX's own pages: FEX
does not check that x64 addresses are canonical, and those pages sit at their host addresses inside the guest's
range. Confining the guest would mean mapping FEX's pages above the 47-bit user range and masking every guest
address. Masking every load and store to 47 bits in FEX's address generation cost 2 to 4% on a memory-heavy loop
(1MB CRC32 table lookups plus 500M xorshift rounds), close to run-to-run noise, and left `test-sample`'s output
unchanged. Moving FEX's pages is not done yet.

**Performance.** On the same loop, HVF-FEX takes 3.3 s, in-process FEX 3.3 s and Unicorn 32.7 s; all three
compute the same result. The VM only costs time at exits, so instruction precision, which makes FEX call the
host before every instruction, is about 90 times slower inside it (46 s against 0.6 s for `test-sample`). The
analyzer therefore leaves precision off for `--backend hvf` unless `--inst-precision` or `--reproducible` is
given.
