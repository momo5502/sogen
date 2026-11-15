#pragma once

#include <arch_emulator.hpp>

namespace sogen::seccomp
{
    EXPORT_SYMBOL std::unique_ptr<arm64_emulator> create_arm64_emulator();
}
