#include <gtest/gtest.h>

#include <module/dyld_shared_cache.hpp>
#include <module/macos_cache_symbols.hpp>

#include <filesystem>

namespace
{
    // Against the host's real cache, and checked against names dyld_info reports for the same image, so
    // the test cannot agree with a wrong implementation by sharing its assumptions.
    TEST(MacosCacheSymbols, ResolvesRealAddressesToTheirExports)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(path))
        {
            GTEST_SKIP() << "no host cache";
        }

        const auto cache =
            sogen::dyld_shared_cache_reader::parse(path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });
        const sogen::macos_cache_symbols symbols{cache};

        const sogen::dyld_cache_image_entry* core_foundation = nullptr;
        for (const auto& image : cache.images())
        {
            if (image.path.ends_with("/CoreFoundation"))
            {
                core_foundation = &image;
                break;
            }
        }

        ASSERT_NE(core_foundation, nullptr) << "CoreFoundation is not in this cache";

        // 0x17A0 is ___CFInitialize, read out of the same image with dyld_info -exports.
        const auto initialize = symbols.lookup(core_foundation->address + 0x17A0);
        ASSERT_TRUE(initialize.has_value());
        EXPECT_EQ(initialize->name, "___CFInitialize");
        EXPECT_EQ(initialize->offset, 0u) << "an exact hit is at offset zero";

        // A few bytes in still names the same function rather than the next one.
        const auto inside = symbols.lookup(core_foundation->address + 0x17A0 + 0x40);
        ASSERT_TRUE(inside.has_value());
        EXPECT_EQ(inside->name, "___CFInitialize");
        EXPECT_EQ(inside->offset, 0x40u);
    }

    // The address that has been blocking every dynamically linked binary. Naming the function it lands in
    // is the whole reason this exists: an offset bracketed between two exports is not a lead.
    TEST(MacosCacheSymbols, NamesTheFunctionCoreFoundationAbortsIn)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(path))
        {
            GTEST_SKIP() << "no host cache";
        }

        const auto cache =
            sogen::dyld_shared_cache_reader::parse(path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });
        const sogen::macos_cache_symbols symbols{cache};

        for (const auto& image : cache.images())
        {
            if (!image.path.ends_with("/CoreFoundation"))
            {
                continue;
            }

            const auto found = symbols.lookup(image.address + 0x2268);
            ASSERT_TRUE(found.has_value()) << "nothing resolves at CoreFoundation+0x2268";

            printf("CoreFoundation+0x2268 is %s+0x%llx\n", found->name.c_str(), static_cast<unsigned long long>(found->offset));
            EXPECT_FALSE(found->name.empty());
            return;
        }

        GTEST_SKIP() << "CoreFoundation is not in this cache";
    }

    TEST(MacosCacheSymbols, AnAddressOutsideEveryImageResolvesToNothing)
    {
        const std::filesystem::path path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(path))
        {
            GTEST_SKIP() << "no host cache";
        }

        const auto cache =
            sogen::dyld_shared_cache_reader::parse(path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });
        const sogen::macos_cache_symbols symbols{cache};

        EXPECT_FALSE(symbols.lookup(0).has_value());
        EXPECT_FALSE(symbols.lookup(cache.shared_region_start() + cache.shared_region_size() + 0x1000).has_value());
    }
}
