#include <gtest/gtest.h>

#include <macos_platform.hpp>

namespace
{
    TEST(MacosPlatform, PageSizeIsSixteenKiB)
    {
        static_assert(sogen::MACOS_PAGE_SIZE == 0x4000);
        EXPECT_EQ(sogen::MACOS_PAGE_SIZE, 0x4000u);
    }

    TEST(MacosPlatform, CommpageBasesAreGuestPageAligned)
    {
        static_assert(sogen::MACOS_COMMPAGE_BASE == 0xFFFFFC000ULL);
        static_assert(sogen::MACOS_COMMPAGE_RO_BASE == 0xFFFFF4000ULL);
        static_assert((sogen::MACOS_COMMPAGE_BASE % sogen::MACOS_PAGE_SIZE) == 0);
        static_assert((sogen::MACOS_COMMPAGE_RO_BASE % sogen::MACOS_PAGE_SIZE) == 0);
        SUCCEED();
    }

    TEST(MacosPlatform, CommpagePagesLieInsideTheNestingRegion)
    {
        constexpr auto nesting_end = sogen::MACOS_COMMPAGE_NESTING_START + sogen::MACOS_COMMPAGE_NESTING_SIZE;
        static_assert(sogen::MACOS_COMMPAGE_RO_BASE >= sogen::MACOS_COMMPAGE_NESTING_START);
        static_assert(sogen::MACOS_COMMPAGE_BASE + sogen::MACOS_COMMPAGE_MAP_SIZE <= nesting_end);
        static_assert(sogen::MACOS_MAX_MMAP_END_EXCL == sogen::MACOS_COMMPAGE_NESTING_START);
        SUCCEED();
    }

    TEST(MacosPlatform, ErrnoValuesMatchDarwinNotLinux)
    {
        EXPECT_EQ(sogen::macos_errno::MACOS_EAGAIN, 35);
        EXPECT_EQ(sogen::macos_errno::MACOS_ENOSYS, 78);
        EXPECT_EQ(sogen::macos_errno::MACOS_ENAMETOOLONG, 63);
        EXPECT_EQ(sogen::macos_errno::MACOS_EDEADLK, 11);
        EXPECT_EQ(sogen::macos_errno::MACOS_EOPNOTSUPP, 102);
    }

    TEST(MacosPlatform, OpenAndMmapFlagsMatchDarwin)
    {
        EXPECT_EQ(sogen::macos_open::MACOS_O_CREAT, 0x200);
        EXPECT_EQ(sogen::macos_open::MACOS_O_CLOEXEC, 0x1000000);
        EXPECT_EQ(sogen::macos_mmap::MACOS_MAP_ANON, 0x1000);
        EXPECT_EQ(sogen::macos_mmap::MACOS_MAP_FIXED, 0x10);
    }
}
