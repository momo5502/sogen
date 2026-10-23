#pragma once

#include <arch_emulator.hpp>
#include <platform/compiler.hpp>

#include <cstddef>
#include <memory>

#ifdef WHP_ARM64_EMULATOR_IMPL
#define WHP_ARM64_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define WHP_ARM64_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::whp
{
#if !SOGEN_BUILD_STATIC
    WHP_ARM64_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<arm64_emulator> create_arm64_emulator(size_t vcpu_count = 1);
}
