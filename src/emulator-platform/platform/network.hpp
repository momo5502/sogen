#pragma once

#include "traits.hpp"
#include "primitives.hpp"

namespace sogen
{

    template <typename Traits>
    struct EMU_WSABUF
    {
        ULONG len;
        EMULATOR_CAST(typename Traits::PVOID, CHAR*) buf;
    };
} // namespace sogen
