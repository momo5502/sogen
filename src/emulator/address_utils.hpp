#pragma once
#include <cstdint>

template <typename T>
T* offset_pointer(void* data, const size_t offset)
{
    return reinterpret_cast<T*>(static_cast<uint8_t*>(data) + offset);
}

template <typename T>
const T* offset_pointer(const void* data, const size_t offset)
{
    return reinterpret_cast<const T*>(static_cast<const uint8_t*>(data) + offset);
}

constexpr bool is_within_start_and_end(const uint64_t value, const uint64_t start, const uint64_t end)
{
    return value >= start && value < end;
}

constexpr bool is_within_start_and_length(const uint64_t value, const uint64_t start, const uint64_t length)
{
    return is_within_start_and_end(value, start, start + length);
}

constexpr bool regions_intersect(const uint64_t start1, const uint64_t end1, const uint64_t start2, const uint64_t end2)
{
    return start1 < end2 && start2 < end1;
}

constexpr bool regions_with_length_intersect(const uint64_t start1, const uint64_t length1, const uint64_t start2,
                                             const uint64_t length2)
{
    return regions_intersect(start1, start1 + length1, start2, start2 + length2);
}

constexpr uint64_t align_down(const uint64_t value, const uint64_t alignment)
{
    return value & ~(alignment - 1);
}

constexpr uint64_t align_up(const uint64_t value, const uint64_t alignment)
{
    return align_down(value + (alignment - 1), alignment);
}

constexpr uint64_t page_align_down(const uint64_t value, const uint64_t page_size = 0x1000)
{
    return align_down(value, page_size);
}

constexpr uint64_t page_align_up(const uint64_t value, const uint64_t page_size = 0x1000)
{
    return align_up(value, page_size);
}
