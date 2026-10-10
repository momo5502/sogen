#pragma once

#include <posix_memory_manager.hpp>

namespace sogen
{
    constexpr uint64_t MACOS_COMMPAGE_ADDRESS = 0x7FFFFFE00000;

    void map_macos_commpage(posix_memory_manager& memory);
}
