#pragma once

#include <posix_memory_manager.hpp>

namespace sogen
{
    constexpr uint64_t MACOS_COMMPAGE_ADDRESS = 0x7FFFFFE00000;

    // MACH_VM_MAX_ADDRESS on x86-64: user mappings end where the commpage begins.
    constexpr uint64_t MACOS_USER_ADDRESS_END = MACOS_COMMPAGE_ADDRESS;

    // The machine the guest sees; host_info reports the same values as the commpage.
    constexpr uint8_t MACOS_CPU_COUNT = 1;
    constexpr uint64_t MACOS_MEMORY_SIZE = 0x200000000;

    void map_macos_commpage(posix_memory_manager& memory);
}
