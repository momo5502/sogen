/*
Design notes:

1. emulator:               the root interface (provides CPU, memory, and hook interfaces).
2. typed_emulator<Traits>: a template that adapts to architecture/bitness via the Traits struct.
3. arch_emulator<Traits>:  a thin layer for architecture-specific logic, things that are shared by all x86 (32/64), or
                           all ARM (32/64), etc.
X. x86_emulator<Traits>:   x86_emulator<Traits> are specialisations for
                           x86 and ARM, parameterised by their respective traits (e.g., x86_64_traits) and stuff :)

1. emulator (cpu_interface, memory_interface, hook_interface)
2.  └── typed_emulator<address_t, register_t, ...>
3.         └── arch_emulator<arch_traits>
              ├── x86_emulator<x86_32_traits>
              ├── x86_emulator<x86_64_traits>
              ├── arm_emulator<arm_32_traits>
              └── arm_emulator<arm_64_traits>
*/

#pragma once
#include "typed_emulator.hpp"
#include "x86_register.hpp"

// --[Core]--------------------------------------------------------------------------

template <typename Traits>
struct arch_emulator : typed_emulator<Traits>
{
};

template <typename Traits>
struct x86_emulator : arch_emulator<Traits>
{
    using register_type = typename Traits::register_type;
    using pointer_type = typename Traits::pointer_type;

    virtual void set_segment_base(register_type base, pointer_type value) = 0;
    virtual void load_gdt(pointer_type address, uint32_t limit) = 0;
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
    sgdt,
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

using x86_64_emulator = x86_emulator<x86_64_traits>;
