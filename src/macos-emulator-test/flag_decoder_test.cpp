#include <gtest/gtest.h>

#include <trace/macos_flag_decoders.hpp>

#include <array>

namespace
{
    TEST(MacosFlagDecoders, OpenAccessModeIsDecodedFromTheLowTwoBits)
    {
        EXPECT_EQ(sogen::format_open_flags(0), "O_RDONLY");
        EXPECT_EQ(sogen::format_open_flags(1), "O_WRONLY");
        EXPECT_EQ(sogen::format_open_flags(2), "O_RDWR");
    }

    TEST(MacosFlagDecoders, OpenFlagsCombineAccessModeAndBits)
    {
        EXPECT_EQ(sogen::format_open_flags(0x201), "O_WRONLY|O_CREAT");
        EXPECT_EQ(sogen::format_open_flags(0x601), "O_WRONLY|O_CREAT|O_TRUNC");
        EXPECT_EQ(sogen::format_open_flags(0x1000000), "O_RDONLY|O_CLOEXEC");
    }

    TEST(MacosFlagDecoders, UnknownOpenBitsSurviveAsHex)
    {
        EXPECT_EQ(sogen::format_open_flags(0x40000000), "O_RDONLY|0x40000000");
    }

    TEST(MacosFlagDecoders, MmapProtectionAndFlags)
    {
        EXPECT_EQ(sogen::format_mmap_protection(0), "PROT_NONE");
        EXPECT_EQ(sogen::format_mmap_protection(3), "PROT_READ|PROT_WRITE");
        EXPECT_EQ(sogen::format_mmap_protection(7), "PROT_READ|PROT_WRITE|PROT_EXEC");
        EXPECT_EQ(sogen::format_mmap_flags(0x1002), "MAP_PRIVATE|MAP_ANON");
        EXPECT_EQ(sogen::format_mmap_flags(0x11), "MAP_SHARED|MAP_FIXED");
    }

    TEST(MacosFlagDecoders, SingleValuedEnums)
    {
        EXPECT_EQ(sogen::format_madvise_advice(5), "MADV_FREE");
        EXPECT_EQ(sogen::format_fcntl_command(3), "F_GETFL");
        EXPECT_EQ(sogen::format_fcntl_command(67), "F_DUPFD_CLOEXEC");
        EXPECT_EQ(sogen::format_seek_whence(2), "SEEK_END");
        EXPECT_EQ(sogen::format_seek_whence(99), "0x63");
    }

    // Every mask in the shipped tables happens to be a single bit, which makes "all bits present" and
    // "any bit present" indistinguishable there. The rule still has to be the strict one for the first
    // composite mask anyone adds, so it is pinned through the public entry point.
    TEST(MacosFlagDecoders, ACompositeMaskMatchesOnlyWhenEveryBitIsPresent)
    {
        constexpr std::array<sogen::macos_flag_bit, 2> bits{{{.mask = 0x3, .name = "BOTH"}, {.mask = 0x4, .name = "OTHER"}}};

        EXPECT_EQ(sogen::format_flag_bits(0x3, bits), "BOTH");
        EXPECT_EQ(sogen::format_flag_bits(0x7, bits), "BOTH|OTHER");
        EXPECT_EQ(sogen::format_flag_bits(0x1, bits), "0x1") << "half of a composite mask is not a match";
    }

    TEST(MacosFlagDecoders, FileModeIsOctal)
    {
        EXPECT_EQ(sogen::format_file_mode(0), "0");
        EXPECT_EQ(sogen::format_file_mode(0644), "0644");
        EXPECT_EQ(sogen::format_file_mode(0755), "0755");
    }

    TEST(MacosFlagDecoders, ErrnoNames)
    {
        EXPECT_EQ(sogen::macos_errno_name(2), "ENOENT");
        EXPECT_EQ(sogen::macos_errno_name(9), "EBADF");
        EXPECT_EQ(sogen::macos_errno_name(22), "EINVAL");
        EXPECT_EQ(sogen::macos_errno_name(78), "ENOSYS");
        EXPECT_EQ(sogen::macos_errno_name(4242), "");
    }
}
