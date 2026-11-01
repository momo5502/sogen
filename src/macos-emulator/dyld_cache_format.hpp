#pragma once

#include <array>
#include <cstdint>

// Subset of dyld's <mach-o/dyld_cache_format.h>; only the header fields the emulator reads are named.
namespace sogen::dyld_cache
{
    struct mapping_and_slide_info
    {
        uint64_t address;
        uint64_t size;
        uint64_t file_offset;
        uint64_t slide_info_file_offset;
        uint64_t slide_info_file_size;
        uint64_t flags;
        uint32_t max_prot;
        uint32_t init_prot;
    };

    struct subcache_entry
    {
        std::array<uint8_t, 16> uuid;
        uint64_t cache_vm_offset;
        std::array<char, 32> file_suffix;
    };

    struct slide_info_v2
    {
        uint32_t version;
        uint32_t page_size;
        uint32_t page_starts_offset;
        uint32_t page_starts_count;
        uint32_t page_extras_offset;
        uint32_t page_extras_count;
        uint64_t delta_mask;
        uint64_t value_add;
    };

    constexpr size_t HEADER_SIZE = 0x228;
    constexpr size_t MAGIC_SIZE = 16;
    constexpr size_t SHARED_REGION_START_OFFSET = 0xE0;
    constexpr size_t SHARED_REGION_SIZE_OFFSET = 0xE8;
    constexpr size_t MAPPING_WITH_SLIDE_OFFSET_OFFSET = 0x138;
    constexpr size_t MAPPING_WITH_SLIDE_COUNT_OFFSET = 0x13C;
    constexpr size_t SUBCACHE_ARRAY_OFFSET_OFFSET = 0x188;
    constexpr size_t SUBCACHE_ARRAY_COUNT_OFFSET = 0x18C;
    constexpr size_t DYNAMIC_DATA_OFFSET_OFFSET = 0x1F0;
    constexpr size_t DYNAMIC_DATA_MAX_SIZE_OFFSET = 0x1F8;

    constexpr uint16_t SLIDE_PAGE_ATTR_EXTRA = 0x8000;
    constexpr uint16_t SLIDE_PAGE_ATTR_NO_REBASE = 0x4000;
    constexpr uint16_t SLIDE_PAGE_ATTR_END = 0x8000;
    constexpr uint16_t SLIDE_PAGE_VALUE_MASK = 0x3FFF;

    static_assert(sizeof(mapping_and_slide_info) == 0x38);
    static_assert(sizeof(subcache_entry) == 0x38);
    static_assert(sizeof(slide_info_v2) == 0x28);
}
