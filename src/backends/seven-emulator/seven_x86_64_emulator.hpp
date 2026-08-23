#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef SEVEN_EMULATOR_IMPL
#define SEVEN_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define SEVEN_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace seven_backend
{
    // The seven backend was authored against a SOGEN fork that exposes the emulator
    // interface types in the global namespace; upstream SOGEN keeps them in `sogen`.
    using namespace sogen;

#if !SOGEN_BUILD_STATIC
    SEVEN_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
}
