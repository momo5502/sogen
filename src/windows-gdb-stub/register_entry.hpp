#pragma once

#include <cstddef>
#include <optional>
#include <x86_register.hpp>
#include <arm64_register.hpp>

namespace sogen
{

    struct register_entry
    {
        int reg;
        std::optional<size_t> expected_size;
        std::optional<size_t> offset;

        constexpr register_entry(const x86_register reg = x86_register::invalid, const std::optional<size_t> expected_size = std::nullopt,
                                 const std::optional<size_t> offset = std::nullopt)
            : reg(static_cast<int>(reg)),
              expected_size(expected_size),
              offset(offset)
        {
        }

        constexpr register_entry(const arm64_register reg, const std::optional<size_t> expected_size = std::nullopt)
            : reg(static_cast<int>(reg)),
              expected_size(expected_size)
        {
        }
    };

} // namespace sogen
