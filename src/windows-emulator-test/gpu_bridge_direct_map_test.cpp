#include <gtest/gtest.h>

#include <gpu_bridge_protocol.hpp>

namespace sogen::test
{
    using gpu_bridge::is_direct_map_range_valid;
    using gpu_bridge::whole_size;

    constexpr uint64_t mapping_size = 0x2000;

    TEST(GpuBridgeDirectMapTest, AcceptsRangesInsideTheMapping)
    {
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, 0, mapping_size));
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, 0x1000, 0x1000));
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, mapping_size - 1, 1));
    }

    TEST(GpuBridgeDirectMapTest, RejectsRangeEndingPastTheMapping)
    {
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 0, mapping_size + 1));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 0x1000, 0x1001));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, mapping_size - 1, 2));
    }

    TEST(GpuBridgeDirectMapTest, RejectsOffsetAtOrPastTheEnd)
    {
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, mapping_size, 1));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, mapping_size, whole_size));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, mapping_size + 0x1000, 0x1000));
        EXPECT_FALSE(is_direct_map_range_valid(0, 0, whole_size));
    }

    TEST(GpuBridgeDirectMapTest, RejectsRangeWhoseEndOverflows)
    {
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 0x1000, ~0ULL - 0x800));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 1, ~0ULL - 1));
    }

    TEST(GpuBridgeDirectMapTest, WholeSizeCoversTheRestOfTheMapping)
    {
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, 0, whole_size));
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, 0x1000, whole_size));
        EXPECT_TRUE(is_direct_map_range_valid(mapping_size, mapping_size - 1, whole_size));
    }

    TEST(GpuBridgeDirectMapTest, RejectsEmptyRange)
    {
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 0, 0));
        EXPECT_FALSE(is_direct_map_range_valid(mapping_size, 0x1000, 0));
    }
}
