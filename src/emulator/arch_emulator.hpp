/*
Design notes:

1. emulator:               the machine interface (provides memory and hook interfaces).
2. typed_emulator<Traits>: a template that adapts to architecture/bitness via the Traits struct.
3. arch_emulator<Traits>:  a thin layer for architecture-specific logic, things that are shared by all x86 (32/64), or
                           all ARM (32/64), etc.
X. x86_emulator<Traits>:   x86_emulator<Traits> are specialisations for
                           x86 and ARM, parameterised by their respective traits (e.g., x86_64_traits) and stuff :)

Virtual CPUs are modelled separately (typed_cpu<Traits> -> x86_cpu<Traits>): a CPU owns register
and run state and delegates memory access to the machine. The machine exposes its CPUs via
get_cpu()/vcpu_count() and currently acts as its own single CPU (index 0) until backends grow
real per-vCPU objects (docs/multi-vcpu-design.md).

1. emulator (memory_interface, hook_interface)          typed_cpu<Traits> (cpu_interface)
2.  └── typed_emulator<address_t, register_t, ...>       └── x86_cpu<x86_64_traits>
3.         └── arch_emulator<arch_traits>                        │
              └── x86_emulator<x86_64_traits> ───────────────────┘ (implements its own CPU 0)
*/

#pragma once
#include "typed_emulator.hpp"
#include "typed_cpu.hpp"
#include "x86_register.hpp"

#include <stdexcept>

namespace sogen
{

    // --[Core]--------------------------------------------------------------------------

    template <typename Traits>
    struct arch_emulator : typed_emulator<Traits>
    {
    };

    template <typename Traits>
    struct x86_cpu : typed_cpu<Traits>
    {
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;

        virtual void set_segment_base(register_type base, pointer_type value) = 0;
        virtual pointer_type get_segment_base(register_type base) = 0;
        virtual void load_gdt(pointer_type address, uint32_t limit) = 0;

        // Add new virtuals at the end of the class so the vtable slots of existing methods never
        // move; this keeps separately built backends (e.g. the dynamically loaded KVM/unicorn/FEX
        // backends) ABI-compatible with the analyzer that calls through this interface.
        //
        // Called once, from module_manager::map_main_modules() right after the process's execution
        // mode is determined (PE-header-driven detection) and before any module - including this
        // process's own executable - is mapped. Backends that execute guest code natively on real
        // x86-64 hardware (KVM/Unicorn/WHP) don't need this - the CPU transparently switches to
        // compatibility mode on the CS segment load alone, so the default implementation is a no-op.
        // A JIT-based backend (FEXCore) can't do that - its bitness is fixed per compiled context -
        // so it uses this to know, as early as possible, whether it needs to stand up a second,
        // 32-bit-mode context.
        virtual void notify_process_bitness(bool /*is_wow64_process*/)
        {
        }

        // Marks [address, address+size) as permanently non-executable for guest *instruction fetch*
        // purposes, regardless of whatever real page permissions it's mapped with (ordinary reads/
        // writes to it are unaffected). Used for sogen's own WoW64 heaven's-gate trampoline
        // (wow64_heaven_gate.hpp): backends that execute guest code natively on real x86-64 hardware
        // (KVM/Unicorn/WHP) genuinely run the trampoline's real machine code (a `retf`/`iretq`
        // sequence performing an actual CS-segment mode switch), so the default no-op is correct for
        // them. A JIT-based backend (FEXCore) cannot execute that sequence at all - its bitness is
        // fixed per compiled Context, so a real mode-switching far return is meaningless to it - and
        // instead needs to intercept guest execution reaching this range *before* any of its bytes
        // are ever JIT-compiled, so it can synthesize the transition's observable effect itself
        // (marshal register state to/from its other-bitness Context and switch which one is active).
        // The natural interception point already exists on every native-execution backend precisely
        // for this shape of problem: reporting an address as not executable via the backend's
        // syscall-handler-facing executable-range query makes FEXCore raise its own synthetic #PF
        // (the same mechanism used for an ordinary DEP violation) before ever attempting to decode
        // guest instructions there.
        virtual void mark_guest_range_permanently_non_executable(pointer_type /*address*/, size_t /*size*/)
        {
        }

        // Identifies which real WoW64 CPU-mode-switch mechanism lives at a registered gate-crossing
        // range, so a JIT backend knows which calling convention to decode when guest execution
        // reaches it (see register_gate_crossing).
        enum class gate_crossing_kind
        {
            // sogen's own synthetic heaven's-gate trampoline (wow64_heaven_gate.hpp, kCodeBase): a
            // 19-byte push/iretq sequence driven with the confirmed convention RAX=target RIP,
            // RBX=target RSP, RCX=target CS, RDX=target SS, current RFLAGS carried through. Used by
            // exception_dispatch.cpp to deliver a 64-bit exception to a thread currently running
            // 32-bit code (Phase D). Fully decodable and implemented on the FEX backend.
            heaven_gate,
            // The real wow64cpu.dll turbo-thunk dispatcher (its TurboDispatchJumpAddressStart export:
            // `mov ecx,eax; shr ecx,0x10; jmp [r15+rcx*8]`). Its convention (EAX dispatch index, r15
            // jump-table base populated by BTCpuProcessInit) differs from the heaven's-gate one and
            // cannot be exercised/verified until wow64cpu.dll actually loads and runs (task #27), so
            // the FEX backend records it but does not yet decode it - see perform_gate_crossing.
            wow64cpu_dispatch,
            // The real wow64cpu.dll forward (64->32) transition function RunSimulatedCode (RVA
            // 0x1650, called in a loop by BTCpuSimulate). It sets up 32-bit execution context and
            // far-jumps into 32-bit compat-mode code (its `mov gs, cx` at RVA 0x16c7 is the exact
            // instruction FEXCore's fixed-bitness 64-bit JIT cannot compile). Registering its entry
            // as a gate makes a JIT backend intercept it *before* any of those bytes are compiled and
            // instead decode the WoW64 CPU-area register block itself, marshaling the 32-bit register
            // file into its 32-bit Context - see perform_gate_crossing. Appended last for vtable-ABI
            // safety (this enum is only passed by value, but keep additions at the end regardless).
            wow64_run_simulated_code,
            // wow64cpu.dll's own BTCpuProcessInit writes this into a dedicated, freshly-r-x'd page
            // (observed at a fixed RVA past the turbo-bop table): a bare `jmp far 0x33:<same page +
            // a few bytes>` (opcode 0xEA - only valid in 16/32-bit mode, undefined in 64-bit long
            // mode, so FEXCore's fixed-bitness JIT can't execute it any more than the other two real
            // transitions above). This is the real Wow64Transition entry point for this ntdll32/
            // wow64cpu.dll build combination - the 32-bit syscall stub's `call fs:[0xC0]` lands
            // directly here, with the syscall number/args already live and its own return address
            // still on the stack - so despite the different encoding (immediate target offset/
            // selector rather than any register convention, and no RSP touch, unlike a call/ret or
            // iretq), reaching it is handled identically to wow64cpu_dispatch's WOW64SVC thunk. See
            // perform_gate_crossing's decode of it.
            far_jmp_bitness_switch,
        };

        // Registers [address, address+size) as a WoW64 bitness gate crossing: a JIT backend
        // intercepts guest execution reaching it (like mark_guest_range_permanently_non_executable,
        // which this implies) and, instead of raising a memory-violation exception, marshals the CPU
        // register file into its other-bitness Context and switches which one is executing - the
        // observable effect of the real hardware CS-segment mode switch that native backends
        // (KVM/Unicorn/WHP) perform transparently, hence the no-op default here.
        virtual void register_gate_crossing(pointer_type /*address*/, size_t /*size*/, gate_crossing_kind /*kind*/)
        {
        }

        // Tells a JIT backend wow64cpu.dll's real TurboDispatchJumpAddressEnd export address - the
        // generic 64-bit dispatch continuation a reverse (32->64) wow64cpu_dispatch gate crossing
        // resumes execution at (see gate_crossing_kind::wow64cpu_dispatch's doc comment). This
        // can't be reliably derived from a fixed RVA offset relative to the image base or to
        // TurboDispatchJumpAddressStart - confirmed empirically to differ across wow64cpu.dll
        // builds/OS versions - so the caller resolves it from the real export table (the same way
        // TurboDispatchJumpAddressStart itself already is) and hands the address over directly. A
        // no-op on native-execution backends (KVM/Unicorn/WHP): they execute the real 64-bit
        // dispatch code directly and never need to resume it synthetically.
        virtual void set_wow64_turbo_dispatch_end(pointer_type /*address*/)
        {
        }
    };

    template <typename Traits>
    struct x86_emulator : arch_emulator<Traits>, x86_cpu<Traits>
    {
        using registers = Traits::register_type;
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;
        using hookable_instructions = Traits::hookable_instructions;

        // Both bases expose a memory surface (the machine's own and the CPU's
        // delegating one); they resolve to the same backend implementation.
        using arch_emulator<Traits>::read_memory;
        using arch_emulator<Traits>::try_read_memory;
        using arch_emulator<Traits>::write_memory;
        using arch_emulator<Traits>::try_write_memory;
        using arch_emulator<Traits>::move_memory;
        using arch_emulator<Traits>::set_memory;

        virtual size_t vcpu_count() const
        {
            return 1;
        }

        virtual x86_cpu<Traits>& get_cpu(const size_t index)
        {
            if (index >= this->vcpu_count())
            {
                throw std::out_of_range("Invalid vCPU index");
            }

            return *this;
        }

        size_t index() const override
        {
            return 0;
        }

        memory_interface& memory() override
        {
            return *this;
        }

        const memory_interface& memory() const override
        {
            return *this;
        }
    };

    template <typename Traits>
    struct arm_emulator : arch_emulator<Traits>
    {
    };

    enum class x86_hookable_instructions
    {
        invalid, // TODO: Get rid of that
        syscall,
        cpuid,
        rdtsc,
        rdtscp,
    };

    // --[x86_64]-------------------------------------------------------------------------

    struct x86_64_traits
    {
        using pointer_type = uint64_t;
        using register_type = x86_register;
        static constexpr register_type instruction_pointer = x86_register::rip;
        static constexpr register_type stack_pointer = x86_register::rsp;
        using hookable_instructions = x86_hookable_instructions;
    };

    using x86_64_cpu = x86_cpu<x86_64_traits>;
    using x86_64_emulator = x86_emulator<x86_64_traits>;

} // namespace sogen
