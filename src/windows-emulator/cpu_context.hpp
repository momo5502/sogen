#pragma once
#include "arch_emulator.hpp"

namespace sogen
{

    namespace cpu_context
    {
        void save(x86_64_cpu& emu, CONTEXT64& context);
        void restore(x86_64_cpu& emu, const CONTEXT64& context);
    }

} // namespace sogen
