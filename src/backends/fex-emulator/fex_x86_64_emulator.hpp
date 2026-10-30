#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef FEX_EMULATOR_IMPL
#define FEX_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define FEX_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::fex
{
#if !SOGEN_BUILD_STATIC
    FEX_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();

    // FEX compiles on the host while its dispatcher and translated code run inside a Hypervisor.framework VM
    // (docs/hvf-backend.md). Only built where the ARM64 Hypervisor.framework backend is.
#if !SOGEN_BUILD_STATIC
    FEX_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_hypervisor_x86_64_emulator();
} // namespace sogen::fex
