#include <gtest/gtest.h>

#include <filesystem/mount_spec.hpp>

namespace sogen::test
{
    TEST(MountSpecTest, ParsesHostGuestAndMode)
    {
        const auto spec = parse_mount_spec(R"(/tmp/session:c:\users\user\appdata:ro)");
        EXPECT_EQ(std::filesystem::path("/tmp/session"), spec.host_path);
        EXPECT_EQ((windows_path{'c', {u"users", u"user", u"appdata"}}), spec.guest_path);
        EXPECT_EQ(filesystem_mode::read_only, spec.mode);

        EXPECT_EQ(filesystem_mode::read_write, parse_mount_spec(R"(/tmp/session:c:\temp:rw)").mode);
        EXPECT_EQ(default_mount_mode, parse_mount_spec(R"(/tmp/session:c:\temp)").mode);
    }

    TEST(MountSpecTest, SplitsFromTheRight)
    {
        const auto windows_host = parse_mount_spec(R"(C:\samples\x:c:\sample:ro)");
        EXPECT_EQ(std::filesystem::path(R"(C:\samples\x)"), windows_host.host_path);
        EXPECT_EQ((windows_path{'c', {u"sample"}}), windows_host.guest_path);

        const auto colon_host = parse_mount_spec("/tmp/a:b:d:/data");
        EXPECT_EQ(std::filesystem::path("/tmp/a:b"), colon_host.host_path);
        EXPECT_EQ((windows_path{'d', {u"data"}}), colon_host.guest_path);

        const auto drive_root = parse_mount_spec("/tmp/a:d:");
        EXPECT_EQ((windows_path{'d', {}}), drive_root.guest_path);
    }

    TEST(MountSpecTest, RejectsInvalidSpecs)
    {
        EXPECT_THROW(parse_mount_spec("/tmp/a"), std::runtime_error);
        EXPECT_THROW(parse_mount_spec(R"(c:\x)"), std::runtime_error);
        EXPECT_THROW(parse_mount_spec(R"(:c:\x)"), std::runtime_error);
        EXPECT_THROW(parse_mount_spec(R"(/tmp/a:c:\x:cow)"), std::runtime_error);
    }

    TEST(MountSpecTest, RejectsAlternateDataStreams)
    {
        EXPECT_THROW(parse_mount_spec(R"(/tmp/a:c:\x.txt:stream)"), std::runtime_error);
        EXPECT_THROW(parse_mount_spec(R"(/tmp/a:c:\x.txt:stream:rw)"), std::runtime_error);
    }

    TEST(MountSpecTest, RenderedSpecsParseBack)
    {
        const mount_spec spec{
            .host_path = "/tmp/a:b",
            .guest_path = windows_path{'c', {u"users", u"public"}},
            .mode = filesystem_mode::read_only,
        };

        const auto parsed = parse_mount_spec(render_mount_spec(spec));
        EXPECT_EQ(spec.host_path, parsed.host_path);
        EXPECT_EQ(spec.guest_path, parsed.guest_path);
        EXPECT_EQ(spec.mode, parsed.mode);
    }
} // namespace sogen::test
