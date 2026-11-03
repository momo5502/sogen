#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sogen::utils::host_watchpoints
{
    // A watched range must be a power of two of at least 8 bytes, aligned to its size (Arm ARM, DBGWCR<n>_EL1.MASK).
    struct range
    {
        uint64_t address{};
        uint64_t size{};

        bool operator==(const range&) const = default;
    };

    size_t capacity();
    bool program(std::span<const range> ranges);
}
