#pragma once
#include "arch_emulator.hpp"
#include <platform/arm64_context.hpp>

namespace sogen
{

    namespace cpu_context
    {
        void save(x86_64_cpu& emu, CONTEXT64& context);
        void restore(x86_64_cpu& emu, const CONTEXT64& context);
        void save(arm64_cpu& emu, arm64_context& context);
        void restore(arm64_cpu& emu, const arm64_context& context);
    }

} // namespace sogen
