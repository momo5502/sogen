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
#include "interface.hpp"
#include "typed_emulator.hpp"
#include "typed_cpu.hpp"
#include "x86_register.hpp"
#include "arm64_register.hpp"

#include <stdexcept>

namespace sogen
{

    // --[Core]--------------------------------------------------------------------------

    template <typename Traits, typename CpuType>
    struct EMULATOR_INTERFACE arch_emulator : typed_emulator<Traits>, CpuType
    {
        using registers = Traits::register_type;
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;
        using hookable_instructions = Traits::hookable_instructions;
        using cpu_type = CpuType;

        // Both bases expose a memory surface (the machine's own and the CPU's
        // delegating one); they resolve to the same backend implementation.
        using typed_emulator<Traits>::read_memory;
        using typed_emulator<Traits>::try_read_memory;
        using typed_emulator<Traits>::write_memory;
        using typed_emulator<Traits>::try_write_memory;
        using typed_emulator<Traits>::move_memory;
        using typed_emulator<Traits>::set_memory;

        size_t vcpu_count() const override
        {
            return 1;
        }

        virtual CpuType& get_cpu(const size_t index)
        {
            if (index >= this->vcpu_count())
            {
                throw std::out_of_range("Invalid vCPU index");
            }

            return *this;
        }

        guest_cpu& get_guest_cpu(size_t index) final
        {
            return this->get_cpu(index);
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
    struct EMULATOR_INTERFACE x86_cpu : typed_cpu<Traits>
    {
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;

        struct descriptor_table_register
        {
            uint64_t base{};
            uint32_t limit{};
        };

        virtual bool read_descriptor_table(int reg, descriptor_table_register& table) = 0;

        virtual void set_segment_base(register_type base, pointer_type value) = 0;
        virtual pointer_type get_segment_base(register_type base) = 0;
        virtual void load_gdt(pointer_type address, uint32_t limit) = 0;

        // Called once before any module is mapped, so a backend that cannot host a WoW64 process
        // can reject it up front instead of failing once compatibility-mode code runs.
        virtual void notify_process_bitness(bool /*is_wow64_process*/)
        {
        }
    };

    template <typename Traits>
    struct EMULATOR_INTERFACE x86_emulator : arch_emulator<Traits, x86_cpu<Traits>>
    {
    };

    template <typename Traits>
    struct EMULATOR_INTERFACE arm_cpu : typed_cpu<Traits>
    {
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;
    };

    template <typename Traits>
    struct EMULATOR_INTERFACE arm_emulator : arch_emulator<Traits, arm_cpu<Traits>>
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

    struct EMULATOR_INTERFACE x86_64_traits
    {
        using pointer_type = uint64_t;
        using register_type = x86_register;
        static constexpr register_type instruction_pointer = x86_register::rip;
        static constexpr register_type stack_pointer = x86_register::rsp;
        using hookable_instructions = x86_hookable_instructions;
    };

    using x86_64_cpu = x86_cpu<x86_64_traits>;
    using x86_64_emulator = x86_emulator<x86_64_traits>;

    enum class arm64_hookable_instructions
    {
        svc,
        // MRS of CNTVCT_EL0, CNTVCTSS_EL0 or PMCCNTR_EL0, which Windows exposes to EL0. The hook receives the destination
        // arm64_register and writes the result itself.
        read_counter,
    };

    // ARM64 interrupt hooks receive the ESR_ELx exception class of a synchronous guest exception,
    // with PC at the faulting instruction.
    enum class arm64_exception_class : int
    {
        undefined_instruction = 0x00,
        // A user PSTATE.SS lets exactly one instruction run; the step is reported with PC at the next one and
        // SS cleared. A step whose instruction raises another exception is dropped.
        software_step = 0x32,
        breakpoint_instruction = 0x3C,
    };

    struct EMULATOR_INTERFACE arm64_traits
    {
        using pointer_type = uint64_t;
        using register_type = arm64_register;
        static constexpr register_type instruction_pointer = arm64_register::pc;
        static constexpr register_type stack_pointer = arm64_register::sp;
        using hookable_instructions = arm64_hookable_instructions;
    };

    using arm64_cpu = arm_cpu<arm64_traits>;
    using arm64_emulator = arm_emulator<arm64_traits>;

} // namespace sogen
