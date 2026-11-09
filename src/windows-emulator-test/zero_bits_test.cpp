#include <gtest/gtest.h>
#include <syscall_utils.hpp>

namespace sogen::test
{
    TEST(ZeroBitsTest, ZeroLeavesTheWholeAddressSpace)
    {
        EXPECT_EQ(highest_address_for_zero_bits(0), MAX_ALLOCATION_END_EXCL - 1);
    }

    TEST(ZeroBitsTest, SmallValuesCountZeroBitsBelowTheHighest32)
    {
        EXPECT_EQ(highest_address_for_zero_bits(1), 0x7FFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(2), 0x3FFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(8), 0xFFFFFFull);
    }

    TEST(ZeroBitsTest, LargerValuesAreMasksWhoseLeadingZeroBitsCount)
    {
        EXPECT_EQ(highest_address_for_zero_bits(0xFFFEFFFF), 0xFFFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(0x7FFEFFFF), 0x7FFFFFFFull);
        EXPECT_EQ(highest_address_for_zero_bits(0x1AAAAAAA), 0x1FFFFFFFull);
    }

    TEST(ZeroBitsTest, AllocationAcceptsWhatLeavesRoomAboveTheFirst64KiB)
    {
        EXPECT_EQ(check_zero_bits_of_allocation(0), STATUS_SUCCESS);
        EXPECT_EQ(check_zero_bits_of_allocation(1), STATUS_SUCCESS);
        EXPECT_EQ(check_zero_bits_of_allocation(15), STATUS_SUCCESS);
        EXPECT_EQ(check_zero_bits_of_allocation(0x10000), STATUS_SUCCESS);
        EXPECT_EQ(check_zero_bits_of_allocation(0xFFFEFFFF), STATUS_SUCCESS);
    }

    TEST(ZeroBitsTest, AllocationFindsNoRangeWhenOnlyTheFirst64KiBRemain)
    {
        EXPECT_EQ(check_zero_bits_of_allocation(16), STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(check_zero_bits_of_allocation(0x8000), STATUS_CONFLICTING_ADDRESSES);
        EXPECT_EQ(check_zero_bits_of_allocation(0xFFFF), STATUS_CONFLICTING_ADDRESSES);
    }

    TEST(ZeroBitsTest, SectionViewsAcceptEncodableConstraintsEvenWhenNoAddressCanFit)
    {
        for (const auto bits : std::array<uint64_t, 10>{0, 1, 16, 17, 20, 21, 0x400, 0x10000, 0x1FFFF, 0xFFFEFFFF})
        {
            EXPECT_EQ(check_zero_bits_of_section(bits), STATUS_SUCCESS) << std::hex << bits;
        }
    }

    TEST(ZeroBitsTest, SectionViewsRejectMoreThan21EffectiveZeroBits)
    {
        for (const uint64_t bits : {22, 23, 31, 32, 33, 63, 0x3FF})
        {
            EXPECT_EQ(check_zero_bits_of_section(bits), STATUS_INVALID_PARAMETER) << std::hex << bits;
        }
    }

    TEST(ZeroBitsTest, AllocationRejectsWhatEndsBelow64KiB)
    {
        EXPECT_EQ(check_zero_bits_of_allocation(17), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(check_zero_bits_of_allocation(32), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(check_zero_bits_of_allocation(0x100), STATUS_INVALID_PARAMETER);
        EXPECT_EQ(check_zero_bits_of_allocation(0x7FFF), STATUS_INVALID_PARAMETER);
    }
}
