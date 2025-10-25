#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wow64::heaven_gate
{
    inline constexpr uint64_t kCodeBase = 0xFF300000ULL;
    inline constexpr std::size_t kCodeSize = 0x1000ULL;

    inline constexpr uint64_t kStackBase = 0xFF400000ULL;
    inline constexpr std::size_t kStackSize = 0x10000ULL;
    inline constexpr uint64_t kStackTop = kStackBase + kStackSize;

    inline constexpr uint16_t kUserCodeSelector = 0x33;
    inline constexpr uint16_t kUserStackSelector = 0x2B;

    inline constexpr std::array<uint8_t, 19> kTrampolineBytes{0x6A, 0x33, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x83, 0x04, 0x24,
                                                              0x05, 0xCB, 0x52, 0x53, 0x9C, 0x51, 0x50, 0x48, 0xCF};
}
