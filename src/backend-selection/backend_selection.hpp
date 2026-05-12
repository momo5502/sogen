#pragma once

#include <cstdint>
#include <memory>
#include <arch_emulator.hpp>

enum class backend_type : uint8_t
{
    auto_select,
    unicorn,
    icicle,
    whp,
};

std::unique_ptr<x86_64_emulator> create_x86_64_emulator(backend_type backend = backend_type::auto_select);
