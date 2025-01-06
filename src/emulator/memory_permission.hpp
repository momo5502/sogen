#pragma once
#include <cstdint>

enum class memory_permission : uint8_t
{
    none = 0,
    read = 1 << 0,
    write = 1 << 1,
    exec = 1 << 2,
    read_write = read | write,
    all = read | write | exec
};

/*****************************************************************************
 *
 ****************************************************************************/

inline constexpr memory_permission operator&(const memory_permission x, const memory_permission y)
{
    return static_cast<memory_permission>(static_cast<uint8_t>(x) & static_cast<uint8_t>(y));
}

inline constexpr memory_permission operator|(const memory_permission x, const memory_permission y)
{
    return static_cast<memory_permission>(static_cast<uint8_t>(x) | static_cast<uint8_t>(y));
}

inline constexpr memory_permission operator^(const memory_permission x, const memory_permission y)
{
    return static_cast<memory_permission>(static_cast<uint8_t>(x) ^ static_cast<uint8_t>(y));
}

inline constexpr memory_permission operator~(memory_permission x)
{
    return static_cast<memory_permission>(~static_cast<uint8_t>(x));
}

inline memory_permission& operator&=(memory_permission& x, const memory_permission y)
{
    x = x & y;
    return x;
}

inline memory_permission& operator|=(memory_permission& x, const memory_permission y)
{
    x = x | y;
    return x;
}

inline memory_permission& operator^=(memory_permission& x, const memory_permission y)
{
    x = x ^ y;
    return x;
}
