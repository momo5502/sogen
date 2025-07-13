#pragma once
#include "arch_emulator.hpp"

namespace cpu_context
{
    void save(x86_64_emulator& emu, CONTEXT64& context);
    void restore(x86_64_emulator& emu, const CONTEXT64& context);
}
