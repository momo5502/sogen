#pragma once

#include <array>
#include <cstdint>

// Windows 11 build 26300.9974 captures of window class sections with 1 to 128 classes.
namespace sogen::test::sdac_capture
{
    struct bucket_sweep_row
    {
        uint32_t window_classes;
        uint32_t section_length;
        uint32_t flags;
        uint32_t search_structure_offset;
        uint32_t bucket_count;
    };

    inline constexpr std::array<bucket_sweep_row, 46> bucket_sweep{{
        {.window_classes = 1, .section_length = 0xd4, .flags = 0x3, .search_structure_offset = 0x0, .bucket_count = 0},
        {.window_classes = 2, .section_length = 0x17c, .flags = 0x3, .search_structure_offset = 0x0, .bucket_count = 0},
        {.window_classes = 3, .section_length = 0x250, .flags = 0x1, .search_structure_offset = 0x224, .bucket_count = 3},
        {.window_classes = 4, .section_length = 0x2fc, .flags = 0x1, .search_structure_offset = 0x2cc, .bucket_count = 3},
        {.window_classes = 5, .section_length = 0x3a8, .flags = 0x1, .search_structure_offset = 0x374, .bucket_count = 3},
        {.window_classes = 6, .section_length = 0x454, .flags = 0x1, .search_structure_offset = 0x41c, .bucket_count = 3},
        {.window_classes = 7, .section_length = 0x500, .flags = 0x1, .search_structure_offset = 0x4c4, .bucket_count = 3},
        {.window_classes = 8, .section_length = 0x5ac, .flags = 0x1, .search_structure_offset = 0x56c, .bucket_count = 3},
        {.window_classes = 9, .section_length = 0x658, .flags = 0x1, .search_structure_offset = 0x614, .bucket_count = 3},
        {.window_classes = 10, .section_length = 0x754, .flags = 0x1, .search_structure_offset = 0x70c, .bucket_count = 3},
        {.window_classes = 11, .section_length = 0x808, .flags = 0x1, .search_structure_offset = 0x7bc, .bucket_count = 3},
        {.window_classes = 12, .section_length = 0x8bc, .flags = 0x1, .search_structure_offset = 0x86c, .bucket_count = 3},
        {.window_classes = 13, .section_length = 0x970, .flags = 0x1, .search_structure_offset = 0x91c, .bucket_count = 3},
        {.window_classes = 14, .section_length = 0xa24, .flags = 0x1, .search_structure_offset = 0x9cc, .bucket_count = 3},
        {.window_classes = 15, .section_length = 0xb18, .flags = 0x1, .search_structure_offset = 0xa7c, .bucket_count = 11},
        {.window_classes = 16, .section_length = 0xbcc, .flags = 0x1, .search_structure_offset = 0xb2c, .bucket_count = 11},
        {.window_classes = 17, .section_length = 0xc80, .flags = 0x1, .search_structure_offset = 0xbdc, .bucket_count = 11},
        {.window_classes = 18, .section_length = 0xd34, .flags = 0x1, .search_structure_offset = 0xc8c, .bucket_count = 11},
        {.window_classes = 19, .section_length = 0xde8, .flags = 0x1, .search_structure_offset = 0xd3c, .bucket_count = 11},
        {.window_classes = 20, .section_length = 0xe9c, .flags = 0x1, .search_structure_offset = 0xdec, .bucket_count = 11},
        {.window_classes = 21, .section_length = 0xf50, .flags = 0x1, .search_structure_offset = 0xe9c, .bucket_count = 11},
        {.window_classes = 22, .section_length = 0x1004, .flags = 0x1, .search_structure_offset = 0xf4c, .bucket_count = 11},
        {.window_classes = 23, .section_length = 0x10b8, .flags = 0x1, .search_structure_offset = 0xffc, .bucket_count = 11},
        {.window_classes = 24, .section_length = 0x116c, .flags = 0x1, .search_structure_offset = 0x10ac, .bucket_count = 11},
        {.window_classes = 25, .section_length = 0x1220, .flags = 0x1, .search_structure_offset = 0x115c, .bucket_count = 11},
        {.window_classes = 26, .section_length = 0x12d4, .flags = 0x1, .search_structure_offset = 0x120c, .bucket_count = 11},
        {.window_classes = 27, .section_length = 0x1388, .flags = 0x1, .search_structure_offset = 0x12bc, .bucket_count = 11},
        {.window_classes = 28, .section_length = 0x143c, .flags = 0x1, .search_structure_offset = 0x136c, .bucket_count = 11},
        {.window_classes = 29, .section_length = 0x14f0, .flags = 0x1, .search_structure_offset = 0x141c, .bucket_count = 11},
        {.window_classes = 30, .section_length = 0x15a4, .flags = 0x1, .search_structure_offset = 0x14cc, .bucket_count = 11},
        {.window_classes = 31, .section_length = 0x1658, .flags = 0x1, .search_structure_offset = 0x157c, .bucket_count = 11},
        {.window_classes = 32, .section_length = 0x170c, .flags = 0x1, .search_structure_offset = 0x162c, .bucket_count = 11},
        {.window_classes = 33, .section_length = 0x17c0, .flags = 0x1, .search_structure_offset = 0x16dc, .bucket_count = 11},
        {.window_classes = 34, .section_length = 0x1874, .flags = 0x1, .search_structure_offset = 0x178c, .bucket_count = 11},
        {.window_classes = 35, .section_length = 0x1928, .flags = 0x1, .search_structure_offset = 0x183c, .bucket_count = 11},
        {.window_classes = 36, .section_length = 0x19dc, .flags = 0x1, .search_structure_offset = 0x18ec, .bucket_count = 11},
        {.window_classes = 37, .section_length = 0x1a90, .flags = 0x1, .search_structure_offset = 0x199c, .bucket_count = 11},
        {.window_classes = 38, .section_length = 0x1b44, .flags = 0x1, .search_structure_offset = 0x1a4c, .bucket_count = 11},
        {.window_classes = 39, .section_length = 0x1bf8, .flags = 0x1, .search_structure_offset = 0x1afc, .bucket_count = 11},
        {.window_classes = 40, .section_length = 0x1cac, .flags = 0x1, .search_structure_offset = 0x1bac, .bucket_count = 11},
        {.window_classes = 48, .section_length = 0x224c, .flags = 0x1, .search_structure_offset = 0x212c, .bucket_count = 11},
        {.window_classes = 50, .section_length = 0x23b4, .flags = 0x1, .search_structure_offset = 0x228c, .bucket_count = 11},
        {.window_classes = 64, .section_length = 0x2d8c, .flags = 0x1, .search_structure_offset = 0x2c2c, .bucket_count = 11},
        {.window_classes = 80, .section_length = 0x38cc, .flags = 0x1, .search_structure_offset = 0x372c, .bucket_count = 11},
        {.window_classes = 100, .section_length = 0x4b40, .flags = 0x1, .search_structure_offset = 0x4680, .bucket_count = 101},
        {.window_classes = 128, .section_length = 0x5fd0, .flags = 0x1, .search_structure_offset = 0x5aa0, .bucket_count = 101},
    }};
}
