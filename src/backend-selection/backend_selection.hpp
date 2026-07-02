#pragma once

#include <cstdint>
#include <memory>
#include <arch_emulator.hpp>

namespace sogen
{

    enum class backend_type : uint8_t
    {
        unicorn,
        icicle,
        whp,
        kvm,
    };

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend = backend_type::unicorn, size_t vcpu_count = 1);
    std::unique_ptr<x86_64_emulator> create_x86_64_emulator_from_environment(size_t vcpu_count = 1);
} // namespace sogen
