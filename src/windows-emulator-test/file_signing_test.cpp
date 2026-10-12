#include "emulation_test_utils.hpp"
#include <utils/file_signing.hpp>
#include <utils/file_handle.hpp>

namespace sogen::test
{
    TEST(FileSigningTest, UnsignedFileDoesNotAcquireTrustedSigningMetadata)
    {
        const utils::file_handle file{std::tmpfile()};
        ASSERT_TRUE(file);
        utils::cached_signing_level info{};
        EXPECT_EQ(STATUS_NOT_FOUND, utils::query_cached_signing_level(file.file_descriptor(), info));
        EXPECT_TRUE(info.thumbprint.empty());
    }

    TEST(FileSigningTest, MicrosoftSigningMeetsAntimalwareRequirement)
    {
        EXPECT_EQ(STATUS_SUCCESS, utils::compare_signing_levels(12, 8));
        EXPECT_EQ(STATUS_SUCCESS, utils::compare_signing_levels(14, 12));
    }

    TEST(FileSigningTest, AuthenticodeDoesNotMeetAntimalwareRequirement)
    {
        constexpr NTSTATUS invalid_image_hash = 0xc0000428;
        EXPECT_EQ(invalid_image_hash, utils::compare_signing_levels(4, 8));
        EXPECT_EQ(invalid_image_hash, utils::compare_signing_levels(0, 12));
        EXPECT_EQ(invalid_image_hash, utils::compare_signing_levels(6, 2));
        EXPECT_EQ(STATUS_SUCCESS, utils::compare_signing_levels(8, 6));
    }

    TEST(FileSigningTest, SigningLevelsUseLowNibble)
    {
        EXPECT_EQ(utils::compare_signing_levels(12, 8), utils::compare_signing_levels(0x1c, 0x18));
    }
}
