#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef WHP_EMULATOR_IMPL
#define WHP_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define WHP_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace whp
{
#if !SOGEN_BUILD_STATIC
    WHP_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
}
