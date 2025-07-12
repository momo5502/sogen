#pragma once
#include "memory_permission.hpp"

enum class memory_permission_ext : uint8_t
{
    none = 0,
    guard = 1 << 0,
};

/*****************************************************************************
 *
 ****************************************************************************/

constexpr memory_permission_ext operator&(const memory_permission_ext x, const memory_permission_ext y)
{
    return static_cast<memory_permission_ext>(static_cast<uint8_t>(x) & static_cast<uint8_t>(y));
}

constexpr memory_permission_ext operator|(const memory_permission_ext x, const memory_permission_ext y)
{
    return static_cast<memory_permission_ext>(static_cast<uint8_t>(x) | static_cast<uint8_t>(y));
}

constexpr memory_permission_ext operator^(const memory_permission_ext x, const memory_permission_ext y)
{
    return static_cast<memory_permission_ext>(static_cast<uint8_t>(x) ^ static_cast<uint8_t>(y));
}

constexpr memory_permission_ext operator~(memory_permission_ext x)
{
    return static_cast<memory_permission_ext>(~static_cast<uint8_t>(x));
}

inline memory_permission_ext& operator&=(memory_permission_ext& x, const memory_permission_ext y)
{
    x = x & y;
    return x;
}

inline memory_permission_ext& operator|=(memory_permission_ext& x, const memory_permission_ext y)
{
    x = x | y;
    return x;
}

inline memory_permission_ext& operator^=(memory_permission_ext& x, const memory_permission_ext y)
{
    x = x ^ y;
    return x;
}

/*****************************************************************************
 *
 ****************************************************************************/

struct nt_memory_permission
{
    memory_permission common;
    memory_permission_ext extended;

    constexpr nt_memory_permission()
        : common(memory_permission::none),
          extended(memory_permission_ext::none)
    {
    }
    constexpr nt_memory_permission(memory_permission common)
        : common(common),
          extended(memory_permission_ext::none)
    {
    }
    constexpr nt_memory_permission(memory_permission common, memory_permission_ext ext)
        : common(common),
          extended(ext)
    {
    }

    // Implicit coercions
    operator memory_permission() const
    {
        return common;
    }
    operator memory_permission_ext() const
    {
        return extended;
    }

    // This just does memberwise equality on each of the members in declaration order
    bool operator==(nt_memory_permission const&) const = default;

    nt_memory_permission& operator=(memory_permission const& y)
    {
        this->common = y;
        return *this;
    }

    constexpr bool is_guarded() const
    {
        return (this->extended & memory_permission_ext::guard) == memory_permission_ext::guard;
    }
};

/*****************************************************************************
 *
 ****************************************************************************/

constexpr nt_memory_permission operator&(const nt_memory_permission x, const memory_permission y)
{
    return nt_memory_permission{x.common & y, x.extended};
}

constexpr nt_memory_permission operator&(const nt_memory_permission x, const memory_permission_ext y)
{
    return nt_memory_permission{x.common, x.extended & y};
}

constexpr nt_memory_permission operator|(const nt_memory_permission x, const memory_permission y)
{
    return nt_memory_permission{x.common | y, x.extended};
}

constexpr nt_memory_permission operator|(const nt_memory_permission x, const memory_permission_ext y)
{
    return nt_memory_permission{x.common, x.extended | y};
}

constexpr nt_memory_permission operator^(const nt_memory_permission x, const memory_permission y)
{
    return nt_memory_permission{x.common ^ y, x.extended};
}

constexpr nt_memory_permission operator^(const nt_memory_permission x, const memory_permission_ext y)
{
    return nt_memory_permission{x.common, x.extended ^ y};
}

inline nt_memory_permission& operator&=(nt_memory_permission& x, const memory_permission y)
{
    x = x & y;
    return x;
}

inline nt_memory_permission& operator&=(nt_memory_permission& x, const memory_permission_ext y)
{
    x = x & y;
    return x;
}

inline nt_memory_permission& operator|=(nt_memory_permission& x, const memory_permission y)
{
    x.common | y;
    return x;
}

inline nt_memory_permission& operator|=(nt_memory_permission& x, const nt_memory_permission y)
{
    x.extended | y;
    return x;
}

inline nt_memory_permission& operator^=(nt_memory_permission& x, const memory_permission y)
{
    x.common ^ y;
    return x;
}

inline nt_memory_permission& operator^=(nt_memory_permission& x, const nt_memory_permission y)
{
    x.extended ^ y;
    return x;
}

/*****************************************************************************
 *
 ****************************************************************************/

inline bool is_guarded(const memory_permission_ext permission)
{
    return (permission & memory_permission_ext::guard) != memory_permission_ext::none;
}
