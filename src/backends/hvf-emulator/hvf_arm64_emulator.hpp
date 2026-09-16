#pragma once

#include <memory>

#include <arch_emulator.hpp>

#include "platform/compiler.hpp"

#ifdef HVF_EMULATOR_IMPL
#define HVF_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define HVF_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::hvf
{
#if !SOGEN_BUILD_STATIC
    HVF_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<arm64_mappable_emulator> create_arm64_emulator();
}
