#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sogen::native_arm64
{
    constexpr uint64_t pstate_nzcv = 0xF0000000;
    constexpr uint64_t pstate_software_step = 1u << 21;

    struct register_state
    {
        std::array<uint64_t, 31> x{};
        uint64_t sp{};
        uint64_t pc{};
        uint64_t pstate{};
        std::array<std::array<std::byte, 16>, 32> vectors{};
        uint32_t fpcr{};
        uint32_t fpsr{};
    };
}
