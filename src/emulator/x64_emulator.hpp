#pragma once
#include "typed_emulator.hpp"
#include "x64_register.hpp"

enum class x64_hookable_instructions
{
    invalid,
    syscall,
    cpuid,
    rdtsc,
    rdtscp,
};

struct x64_emulator
    : typed_emulator<uint64_t, x64_register, x64_register::rip, x64_register::rsp, x64_hookable_instructions>
{
    virtual void set_segment_base(x64_register base, pointer_type value) = 0;
};
