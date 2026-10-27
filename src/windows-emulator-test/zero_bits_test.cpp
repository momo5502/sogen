#include <gtest/gtest.h>
#include <syscall_utils.hpp>

namespace sogen::test
{
    TEST(ZeroBitsTest, ZeroLeavesTheWholeAddressSpace)
    {
        EXPECT_EQ(highest_address_for_zero_bits(0), MAX_ALLOCATION_END_EXCL - 1);
    }

    TEST(ZeroBitsTest, SmallValuesCountHighBitsOfThe64BitAddress)
    {
        EXPECT_EQ(highest_address_for_zero_bits(1), MAX_ALLOCATION_END_EXCL - 1);
        EXPECT_EQ(highest_address_for_zero_bits(20), 0xFFFFFFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(32), 0xFFFFFFFFull);
    }

    TEST(ZeroBitsTest, LargerValuesAreMasksWhoseLeadingZeroBitsCount)
    {
        EXPECT_EQ(highest_address_for_zero_bits(0xFFFEFFFF), 0xFFFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(0x7FFEFFFF), 0x7FFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(0x1AAAAAAA), 0x1FFFFFFFull);
    }
}
