#pragma once

#include <cstddef>
#include <optional>
#include <x86_register.hpp>

struct register_entry
{
    x86_register reg;
    std::optional<size_t> expected_size;
    std::optional<size_t> offset;

    register_entry(const x86_register reg = x86_register::invalid, const std::optional<size_t> expected_size = std::nullopt,
                   const std::optional<size_t> offset = std::nullopt)
        : reg(reg),
          expected_size(expected_size),
          offset(offset)
    {
    }
};
