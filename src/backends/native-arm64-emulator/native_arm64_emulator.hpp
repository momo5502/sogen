#pragma once

#include <arch_emulator.hpp>

namespace sogen::native_arm64
{
    EXPORT_SYMBOL std::unique_ptr<arm64_emulator> create_arm64_emulator();
}
