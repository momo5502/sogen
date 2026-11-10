#pragma once

#include <arch_emulator.hpp>
#include <platform/compiler.hpp>

#include <cstddef>
#include <memory>

#ifdef HVF_X86_64_EMULATOR_IMPL
#define HVF_X86_64_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define HVF_X86_64_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::hvf
{
    // True when the host supports Hypervisor.framework and this process holds the
    // com.apple.security.hypervisor entitlement.
#if !SOGEN_BUILD_STATIC
    HVF_X86_64_EMULATOR_DLL_STORAGE
#endif
    bool is_available();

#if !SOGEN_BUILD_STATIC
    HVF_X86_64_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(size_t vcpu_count = 1);
}
