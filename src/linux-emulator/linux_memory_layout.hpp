#pragma once

#include <posix_memory_manager.hpp>

namespace sogen
{
    constexpr uint64_t LINUX_PAGE_SIZE = 0x1000ULL;

    // Allow higher virtual addresses used by modern Linux loaders/runtime setups
    // (e.g. 5-level paging style layouts) instead of limiting to classic 47-bit.
    constexpr memory_layout LINUX_MEMORY_LAYOUT{
        .allocation_granularity = 0x1000ULL,
        .min_mmap_address = 0x0000000000010000ULL,
        .max_mmap_end = 0x0001000000000000ULL,
        .default_mmap_base = 0x7f0000000000ULL,
    };
}
