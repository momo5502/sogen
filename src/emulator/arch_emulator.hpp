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
        // A JIT-based backend (FEXCore) is fixed-bitness per compiled context and only stands up the
        // 64-bit one, so it uses this to reject a WoW64 (32-bit) process with a clear error up
        // front, before its first block would be mis-decoded as 64-bit code.
        virtual void notify_process_bitness(bool /*is_wow64_process*/)
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
