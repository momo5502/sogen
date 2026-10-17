#pragma once

#include <cstdint>
#include <array>

namespace sogen
{
    inline constexpr uint32_t arm64_context_architecture = 0x00400000;
    inline constexpr uint32_t arm64_context_control = arm64_context_architecture | 1;
    inline constexpr uint32_t arm64_context_integer = arm64_context_architecture | 2;
    inline constexpr uint32_t arm64_context_floating_point = arm64_context_architecture | 4;
    inline constexpr uint32_t arm64_context_debug_registers = arm64_context_architecture | 8;
    inline constexpr uint32_t arm64_context_full = arm64_context_control | arm64_context_integer | arm64_context_floating_point;
    inline constexpr uint32_t arm64_context_all = arm64_context_full | arm64_context_debug_registers;

    // Layout of the public SDK ARM64_NT_CONTEXT, independent of the host's CONTEXT definition.
    struct alignas(16) arm64_context
    {
        uint32_t ContextFlags{};
        uint32_t Cpsr{};
        std::array<uint64_t, 31> X{};
        uint64_t Sp{};
        uint64_t Pc{};

        struct vector_register
        {
            uint64_t Low{};
            int64_t High{};
        };

        std::array<vector_register, 32> V{};
        uint32_t Fpcr{};
        uint32_t Fpsr{};
        std::array<uint32_t, 8> Bcr{};
        std::array<uint64_t, 8> Bvr{};
        std::array<uint32_t, 2> Wcr{};
        std::array<uint64_t, 2> Wvr{};
    };
}
