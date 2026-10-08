#pragma once

#include <cstdint>
#include <type_traits>

namespace sogen
{
    template <typename Handle>
    uint64_t pack_vulkan_handle(const Handle value)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(value));
        }
        else
        {
            return static_cast<uint64_t>(value);
        }
    }

    template <typename Handle>
    Handle unpack_vulkan_handle(const uint64_t value)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(value));
        }
        else
        {
            return static_cast<Handle>(value);
        }
    }
}
