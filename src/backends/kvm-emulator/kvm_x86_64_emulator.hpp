#pragma once

#include <memory>
#include <arch_emulator.hpp>
#include "platform/platform.hpp"

#ifdef KVM_EMULATOR_IMPL
#define KVM_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define KVM_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace sogen::kvm
{
#if !SOGEN_BUILD_STATIC
    KVM_EMULATOR_DLL_STORAGE
#endif
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator();
} // namespace sogen::kvm
