#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <macos_root_manifest.hpp>

#include "fixture_utils.hpp"

namespace
{
    std::vector<std::byte> bytes_of(const std::string_view text)
    {
        std::vector<std::byte> data{};
        data.reserve(text.size());
        for (const char ch : text)
        {
            data.push_back(static_cast<std::byte>(ch));
        }
        return data;
    }

    constexpr std::string_view REAL_MANIFEST = R"(schema=1
tool=grab-macos-root
tool_version=1
created=2026-08-18T09:41:12Z
mode=copy
arch=arm64e
product_name=macOS
product_version=26.6.1
build_version=25G76
kernel_version=Darwin Kernel Version 25.6.0: Sat Jul 11 15:27:26 PDT 2026; root:xnu-12377.161.13~4/RELEASE_ARM64_T6031
hardware_model=Mac15,11
cache_guest_dir=/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld
cache_base_name=dyld_shared_cache_arm64e
cache_uuid=6981b4b9e09034ce862a41516efef2ad
cache_file_count=13
cache_total_bytes=5820841984
dyld_guest_path=/usr/lib/dyld
dyld_sha256=3f4bd9695ac12e12a2a6f31294fd3d2af01960aa06514ffa086491342526724c
optional=icu
optional=timezone
optional=system_version
)";

    TEST(MacosRootManifest, ParsesAManifestProducedByTheExtractionScript)
    {
        const auto manifest = sogen::parse_macos_root_manifest(bytes_of(REAL_MANIFEST));
        ASSERT_TRUE(manifest.has_value());
        EXPECT_EQ(manifest->schema, 1u);
        EXPECT_EQ(manifest->mode, "copy");
        EXPECT_EQ(manifest->arch, "arm64e");
        EXPECT_EQ(manifest->product_version, "26.6.1");
        EXPECT_EQ(manifest->build_version, "25G76");
        EXPECT_EQ(manifest->hardware_model, "Mac15,11");
        EXPECT_EQ(manifest->cache_guest_dir, "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld");
        EXPECT_EQ(manifest->cache_base_name, "dyld_shared_cache_arm64e");
        EXPECT_EQ(manifest->cache_uuid, "6981b4b9e09034ce862a41516efef2ad");
        EXPECT_EQ(manifest->cache_file_count, 13u);
        EXPECT_EQ(manifest->cache_total_bytes, 5820841984ull);
        EXPECT_EQ(manifest->dyld_guest_path, "/usr/lib/dyld");
        ASSERT_EQ(manifest->optional_items.size(), 3u);
        EXPECT_EQ(manifest->optional_items[0], "icu");
        EXPECT_EQ(manifest->optional_items[2], "system_version");
        EXPECT_NE(manifest->kernel_version.find("xnu-12377"), std::string::npos);
    }

    TEST(MacosRootManifest, IgnoresBlankLinesCommentsAndUnknownKeys)
    {
        const auto manifest = sogen::parse_macos_root_manifest(bytes_of("schema=1\n\n# a comment\nfuture_key=whatever\narch=arm64e\n"));
        ASSERT_TRUE(manifest.has_value());
        EXPECT_EQ(manifest->arch, "arm64e");
    }

    TEST(MacosRootManifest, RejectsAManifestWithoutASchema)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("arch=arm64e\n")).has_value());
    }

    TEST(MacosRootManifest, RejectsAnUnsupportedSchema)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=99\narch=arm64e\n")).has_value());
    }

    TEST(MacosRootManifest, RejectsEmptyAndOversizedInput)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest({}).has_value());

        const std::vector<std::byte> oversized(sogen::MAX_MANIFEST_SIZE + 1, std::byte{'x'});
        EXPECT_FALSE(sogen::parse_macos_root_manifest(oversized).has_value());
    }

    TEST(MacosRootManifest, RejectsGarbageWithoutCrashing)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("\x01\x02\x03 nonsense")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=notanumber\n")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ncache_file_count=notanumber\n")).has_value());
    }

    TEST(MacosRootManifest, RoundTripsThroughItsOwnFormatter)
    {
        const auto original = sogen::parse_macos_root_manifest(bytes_of(REAL_MANIFEST));
        ASSERT_TRUE(original.has_value());

        const auto text = sogen::format_macos_root_manifest(*original);
        const auto reparsed = sogen::parse_macos_root_manifest(bytes_of(text));
        ASSERT_TRUE(reparsed.has_value());

        EXPECT_EQ(reparsed->build_version, original->build_version);
        EXPECT_EQ(reparsed->cache_uuid, original->cache_uuid);
        EXPECT_EQ(reparsed->cache_file_count, original->cache_file_count);
        EXPECT_EQ(reparsed->cache_total_bytes, original->cache_total_bytes);
        EXPECT_EQ(reparsed->optional_items, original->optional_items);
    }

    TEST(MacosRootManifest, LoadsFromARootDirectory)
    {
        const sogen::test::temp_directory dir{"manifest-load"};
        {
            std::ofstream stream{dir.path() / sogen::MACOS_ROOT_MANIFEST_NAME, std::ios::binary | std::ios::trunc};
            stream << REAL_MANIFEST;
        }

        const auto manifest = sogen::load_macos_root_manifest(dir.path());
        ASSERT_TRUE(manifest.has_value());
        EXPECT_EQ(manifest->build_version, "25G76");
    }

    TEST(MacosRootManifest, ReturnsNothingWhenTheManifestIsAbsent)
    {
        const sogen::test::temp_directory dir{"manifest-absent"};
        EXPECT_FALSE(sogen::load_macos_root_manifest(dir.path()).has_value());
    }

    TEST(MacosRootManifest, DescribesItselfInOneLine)
    {
        const auto manifest = sogen::parse_macos_root_manifest(bytes_of(REAL_MANIFEST));
        ASSERT_TRUE(manifest.has_value());

        const auto text = sogen::describe_macos_root_manifest(*manifest);
        EXPECT_NE(text.find("26.6.1"), std::string::npos);
        EXPECT_NE(text.find("25G76"), std::string::npos);
        EXPECT_NE(text.find("arm64e"), std::string::npos);
        EXPECT_EQ(text.find('\n'), std::string::npos);
    }

    TEST(MacosRootManifest, RejectsAnOversizedManifestThatWouldOtherwiseParse)
    {
        std::string text{"schema=1\narch=arm64e\n"};
        while (text.size() <= sogen::MAX_MANIFEST_SIZE)
        {
            text += "# padding\n";
        }

        ASSERT_GT(text.size(), sogen::MAX_MANIFEST_SIZE);
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of(text)).has_value());

        text.resize(sogen::MAX_MANIFEST_SIZE);
        EXPECT_TRUE(sogen::parse_macos_root_manifest(bytes_of(text)).has_value());
    }

    TEST(MacosRootManifest, RejectsAControlCharacterInsideAWellFormedValue)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\narch=arm\x01"
                                                               "64e\n"))
                         .has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\narch=arm\r64e\n")).has_value());
    }

    TEST(MacosRootManifest, RejectsALineThatIsNotAKeyValuePair)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ngarbage\n")).has_value());
    }

    TEST(MacosRootManifest, RejectsTrailingJunkAfterANumber)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1x\n")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ncache_file_count=12abc\n")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ncache_total_bytes=1 2\n")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ncache_file_count=-1\n")).has_value());
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=1\ncache_file_count=99999999999\n")).has_value());
    }

    TEST(MacosRootManifest, RejectsASchemaThatIsPresentButZero)
    {
        EXPECT_FALSE(sogen::parse_macos_root_manifest(bytes_of("schema=0\narch=arm64e\n")).has_value());
    }

    TEST(MacosRootManifest, RefusesAManifestPathNamingACharacterDevice)
    {
        const std::filesystem::path device{"/dev/zero"};
        if (!std::filesystem::exists(device))
        {
            GTEST_SKIP() << "no /dev/zero on this host";
        }

        const sogen::test::temp_directory dir{"manifest-device"};

        std::error_code error{};
        std::filesystem::create_symlink(device, dir.path() / sogen::MACOS_ROOT_MANIFEST_NAME, error);
        if (error)
        {
            GTEST_SKIP() << "the host refused to create a symlink in the scratch root";
        }

        EXPECT_FALSE(sogen::load_macos_root_manifest(dir.path()).has_value());
    }
}
