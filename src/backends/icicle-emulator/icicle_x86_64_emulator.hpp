#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef ICICLE_EMULATOR_IMPL
#define ICICLE_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define ICICLE_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace icicle
{
#if !MOMO_BUILD_AS_LIBRARY
    ICICLE_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
}
