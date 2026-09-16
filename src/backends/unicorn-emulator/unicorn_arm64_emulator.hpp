#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define UNICORN_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::unicorn
{
#if !SOGEN_BUILD_STATIC
    UNICORN_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<arm64_mappable_emulator> create_arm64_emulator();
} // namespace sogen::unicorn
