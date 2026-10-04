/*
Design notes:

1. emulator:               the machine interface (provides memory and hook interfaces).
2. typed_emulator<Traits>: a template that adapts to architecture/bitness via the Traits struct.
3. arch_emulator<Traits>:  a thin layer for architecture-specific logic, things that are shared by all x86 (32/64), or
                           all ARM (32/64), etc.
X. x86_emulator<Traits>/   x86_emulator<Traits> and arm64_emulator<Traits> are specialisations for
   arm64_emulator<Traits>: x86 and ARM64, parameterised by their respective traits (e.g., x86_64_traits,
                           arm64_traits) and stuff :)

Virtual CPUs are modelled separately (typed_cpu<Traits> -> x86_cpu<Traits>/arm64_cpu<Traits>): a CPU
owns register and run state and delegates memory access to the machine. The machine exposes its CPUs
via get_cpu()/vcpu_count() and currently acts as its own single CPU (index 0) until backends grow
real per-vCPU objects (docs/multi-vcpu-design.md).

1. emulator (memory_interface, hook_interface)          typed_cpu<Traits> (cpu_interface)
2.  └── typed_emulator<address_t, register_t, ...>       ├── x86_cpu<x86_64_traits>
3.         └── arch_emulator<arch_traits>                └── arm64_cpu<arm64_traits>
              ├── x86_emulator<x86_64_traits>    (implements its own CPU 0, i.e. x86_cpu)
              └── arm64_emulator<arm64_traits>   (implements its own CPU 0, i.e. arm64_cpu)
*/

#pragma once
#include "typed_emulator.hpp"
#include "typed_cpu.hpp"
#include "x86_register.hpp"
#include "arm64_register.hpp"

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

        // Called once before any module is mapped. Backends running on real x86-64 hardware ignore
        // this; the CPU switches to compatibility mode on the CS load alone. FEXCore compiles for a
        // fixed bitness and only stands up the 64-bit context, so it uses this to reject a WoW64
        // process up front instead of mis-decoding its first block as 64-bit code.
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

    // Mirrors uc_arm64_pauth_key. The instruction key A/B split is what dyld's autda and autib pick
    // between, so the caller has to name the same one the guest will.
    enum class arm64_pauth_key : uint8_t
    {
        instruction_a = 0,
        instruction_b = 1,
        data_a = 2,
        data_b = 3,
        generic = 4,
    };

    template <typename Traits>
    struct arm64_cpu : typed_cpu<Traits>
    {
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;

        virtual void set_thread_pointer(pointer_type value) = 0;
        virtual pointer_type get_thread_pointer() = 0;

        // System registers cannot travel over read_raw_register: the (op0, op1, CRn, CRm, op2) encoding has to
        // be handed *in* through the destination buffer, which read_raw_register zeroes before reading.
        virtual uint64_t read_system_register(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) = 0;

        // An emulated kernel has to be able to sign a pointer the way the emulated CPU will authenticate
        // it. A real kernel does this when it applies a shared cache's slide info: the pointers it
        // writes have to survive the guest's own autda, and a plain address does not -- authenticating
        // one poisons it and the next dereference faults.
        virtual bool sign_pointer(pointer_type& pointer, arm64_pauth_key key, uint64_t discriminator) = 0;

        // Pointer authentication is a per-process mode on Darwin, not a machine-wide one. A main
        // executable whose cpusubtype lacks the arm64e ptrauth ABI runs with the keys disabled, which
        // turns every pac*/aut* in the arm64e shared cache into a no-op. Measured on 25G76: such a
        // process sees raw pointers everywhere, its own constant-CFString isa and the cache's class and
        // superclass slots alike, while the same source built arm64e sees a signature in all three.
        virtual void set_pointer_authentication(bool enabled) = 0;
    };

    template <typename Traits>
    struct arm64_emulator : arch_emulator<Traits>, arm64_cpu<Traits>
    {
        using registers = Traits::register_type;
        using register_type = Traits::register_type;
        using pointer_type = Traits::pointer_type;
        using hookable_instructions = Traits::hookable_instructions;

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

        virtual arm64_cpu<Traits>& get_cpu(const size_t index)
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

    enum class arm64_hookable_instructions
    {
        invalid,
        svc,
        brk,
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

    // --[arm64]--------------------------------------------------------------------------

    struct arm64_traits
    {
        using pointer_type = uint64_t;
        using register_type = arm64_register;
        static constexpr register_type instruction_pointer = arm64_register::pc;
        static constexpr register_type stack_pointer = arm64_register::sp;
        using hookable_instructions = arm64_hookable_instructions;
    };

    using arm64_64_cpu = arm64_cpu<arm64_traits>;
    using arm64_64_emulator = arm64_emulator<arm64_traits>;

    // memory_interface keeps the address space virtuals private so only a memory manager can reshape
    // the guest. An emulator driven without a manager in front of it has to map its own memory, so it
    // republishes the override.
    struct arm64_mappable_emulator : arm64_64_emulator
    {
        void map_memory(uint64_t address, size_t size, memory_permission permissions) override = 0;
        void map_host_memory(uint64_t address, size_t size, void* host_pointer, memory_permission permissions) override = 0;
        void unmap_memory(uint64_t address, size_t size) override = 0;
        void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override = 0;
        void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override = 0;
    };

} // namespace sogen
