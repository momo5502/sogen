#pragma once

#include "std_include.hpp"

#include "macos_memory_manager.hpp"

#include <string>

namespace sogen
{
    struct macos_memory_report
    {
        uint64_t guest_committed_bytes{};
        uint64_t guest_reserved_bytes{};
        uint64_t guest_region_count{};
        uint64_t host_heap_bytes{};
        uint64_t cache_resident_bytes{};
    };

    uint64_t query_host_heap_bytes();
    macos_memory_report collect_macos_memory_report(const macos_memory_manager& memory);
    std::string format_macos_memory_report(const macos_memory_report& report);
}
