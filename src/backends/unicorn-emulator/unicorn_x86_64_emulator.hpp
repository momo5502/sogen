#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define UNICORN_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace unicorn
{
#if !MOMO_BUILD_AS_LIBRARY
    UNICORN_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
}
