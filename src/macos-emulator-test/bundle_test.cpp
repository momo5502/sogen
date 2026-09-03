#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>

#include <macos_bundle.hpp>
#include <plist.hpp>

#include "fixture_utils.hpp"

namespace
{
    void write_text(const std::filesystem::path& path, const std::string& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << text;
    }

    std::string info_plist(const std::string& executable)
    {
        return R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>)" +
               executable + R"(</string>
	<key>CFBundleIdentifier</key>
	<string>dev.sogen.test</string>
</dict>
</plist>
)";
    }

    std::filesystem::path make_bundle(const std::filesystem::path& parent, const std::string& name, const std::string& executable,
                                      const bool with_plist = true)
    {
        const auto bundle = parent / (name + ".app");
        std::filesystem::create_directories(bundle / "Contents" / "MacOS");
        if (with_plist)
        {
            write_text(bundle / "Contents" / "Info.plist", info_plist(executable));
        }
        write_text(bundle / "Contents" / "MacOS" / executable, "\xcf\xfa\xed\xfe payload");
        return bundle;
    }

    TEST(MacosBundle, ResolvesTheExecutableNamedByTheInfoPlist)
    {
        const sogen::test::temp_directory dir{"bundle-basic"};
        const auto bundle = make_bundle(dir.path(), "Widget", "WidgetBinary");

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->bundle_name, "Widget.app");
        EXPECT_EQ(resolved->executable_name, "WidgetBinary");
        EXPECT_EQ(resolved->identifier, "dev.sogen.test");
        EXPECT_EQ(resolved->executable, std::filesystem::weakly_canonical(bundle) / "Contents" / "MacOS" / "WidgetBinary");
        EXPECT_TRUE(error.empty());
    }

    TEST(MacosBundle, FallsBackToTheBundleNameWhenTheInfoPlistIsMissing)
    {
        const sogen::test::temp_directory dir{"bundle-noplist"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget", false);

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->executable_name, "Widget");
        EXPECT_TRUE(resolved->identifier.empty());
    }

    TEST(MacosBundle, FallsBackToTheBundleNameWhenTheInfoPlistIsUnparsable)
    {
        const sogen::test::temp_directory dir{"bundle-badplist"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget", false);
        write_text(bundle / "Contents" / "Info.plist", "not a plist");

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->executable_name, "Widget");
    }

    TEST(MacosBundle, RejectsAnExecutableNameThatEscapesTheBundle)
    {
        const sogen::test::temp_directory dir{"bundle-escape"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        write_text(bundle / "Contents" / "Info.plist", info_plist("../../../../usr/bin/env"));

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        EXPECT_FALSE(resolved.has_value());
        EXPECT_NE(error.find("CFBundleExecutable"), std::string::npos);
    }

    TEST(MacosBundle, RejectsDotAndDotDotAsExecutableNames)
    {
        for (const std::string& name : {std::string{"."}, std::string{".."}, std::string{""}})
        {
            const sogen::test::temp_directory dir{"bundle-dots"};
            const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
            write_text(bundle / "Contents" / "Info.plist", info_plist(name));

            std::string error{};
            EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value()) << "name '" << name << "'";
        }
    }

    TEST(MacosBundle, RejectsAMissingExecutable)
    {
        const sogen::test::temp_directory dir{"bundle-missing"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove(bundle / "Contents" / "MacOS" / "Widget");

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        EXPECT_FALSE(resolved.has_value());
        EXPECT_NE(error.find("Contents/MacOS"), std::string::npos);
    }

    TEST(MacosBundle, RejectsAnExecutableThatIsADirectory)
    {
        const sogen::test::temp_directory dir{"bundle-dir-exec"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove(bundle / "Contents" / "MacOS" / "Widget");
        std::filesystem::create_directories(bundle / "Contents" / "MacOS" / "Widget");

        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value());
    }

    TEST(MacosBundle, RejectsAPathThatIsNotABundle)
    {
        const sogen::test::temp_directory dir{"bundle-not"};
        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(dir.path(), error).has_value());
        EXPECT_FALSE(sogen::resolve_app_bundle(dir.path() / "absent.app", error).has_value());
    }

    TEST(MacosBundle, RecognisesBundlePathsCaseInsensitively)
    {
        EXPECT_TRUE(sogen::is_app_bundle_path("/Applications/Widget.app"));
        EXPECT_TRUE(sogen::is_app_bundle_path("/Applications/Widget.APP"));
        EXPECT_FALSE(sogen::is_app_bundle_path("/Applications/Widget"));
        EXPECT_FALSE(sogen::is_app_bundle_path("/Applications/Widget.appx"));
    }

    TEST(MacosBundle, FindsTheEnclosingBundleOfAnExecutable)
    {
        const auto enclosing = sogen::enclosing_app_bundle("/Volumes/Disk/Widget.app/Contents/MacOS/Widget");
        ASSERT_TRUE(enclosing.has_value());
        EXPECT_EQ(*enclosing, std::filesystem::path{"/Volumes/Disk/Widget.app"});

        EXPECT_FALSE(sogen::enclosing_app_bundle("/usr/bin/env").has_value());
        EXPECT_FALSE(sogen::enclosing_app_bundle("/Volumes/Disk/Widget.app/Contents/Resources/data").has_value());
    }

    // The bundle root is what gets mapped read-only into the guest, and every containment decision below
    // it canonicalizes, so a lexical root would name a directory the rest of the resolver never inspects.
    TEST(MacosBundle, ResolvesTheBundleRootToItsCanonicalPath)
    {
        const sogen::test::temp_directory dir{"bundle-symlinked-root"};
        const auto real_bundle = make_bundle(dir.path() / "real", "Widget", "Widget");

        const auto link = dir.path() / "Linked.app";
        std::error_code code{};
        std::filesystem::create_directory_symlink(real_bundle, link, code);
        ASSERT_FALSE(code) << code.message();

        const auto canonical_bundle = std::filesystem::weakly_canonical(real_bundle, code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(link, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->bundle_root, canonical_bundle);
        EXPECT_NE(resolved->bundle_root, link.lexically_normal());
        EXPECT_EQ(resolved->bundle_name, "Widget.app");
        EXPECT_EQ(resolved->executable, canonical_bundle / "Contents" / "MacOS" / "Widget");
    }

    TEST(MacosBundle, RejectsAnExecutableSymlinkedOutsideTheBundle)
    {
        const sogen::test::temp_directory dir{"bundle-symlink-out"};
        const auto outside = dir.path() / "outside" / "payload";
        write_text(outside, "\xcf\xfa\xed\xfe outside");

        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove(bundle / "Contents" / "MacOS" / "Widget");

        std::error_code code{};
        std::filesystem::create_symlink(outside, bundle / "Contents" / "MacOS" / "Widget", code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value());
        EXPECT_NE(error.find("outside the bundle"), std::string::npos) << error;
    }

    // "Widget.app.backup" shares a string prefix with "Widget.app" but is not inside it. Containment
    // that compares strings rather than path components accepts this.
    TEST(MacosBundle, RejectsAnExecutableInASiblingSharingTheBundleNameAsAPrefix)
    {
        const sogen::test::temp_directory dir{"bundle-sibling"};
        const auto sibling = dir.path() / "Widget.app.backup" / "payload";
        write_text(sibling, "\xcf\xfa\xed\xfe sibling");

        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove(bundle / "Contents" / "MacOS" / "Widget");

        std::error_code code{};
        std::filesystem::create_symlink(sibling, bundle / "Contents" / "MacOS" / "Widget", code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value());
        EXPECT_NE(error.find("outside the bundle"), std::string::npos) << error;
    }

    TEST(MacosBundle, AcceptsAnExecutableSymlinkThatStaysInsideTheBundle)
    {
        const sogen::test::temp_directory dir{"bundle-symlink-in"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove(bundle / "Contents" / "MacOS" / "Widget");

        const auto inside = bundle / "Contents" / "Resources" / "real";
        write_text(inside, "\xcf\xfa\xed\xfe inside");

        std::error_code code{};
        std::filesystem::create_symlink(inside, bundle / "Contents" / "MacOS" / "Widget", code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        EXPECT_TRUE(sogen::resolve_app_bundle(bundle, error).has_value()) << error;
    }

    TEST(MacosBundle, RejectsAMacOsDirectorySymlinkedOutsideTheBundle)
    {
        const sogen::test::temp_directory dir{"bundle-dir-symlink"};
        const auto elsewhere = dir.path() / "elsewhere";
        write_text(elsewhere / "Widget", "\xcf\xfa\xed\xfe elsewhere");

        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        std::filesystem::remove_all(bundle / "Contents" / "MacOS");

        std::error_code code{};
        std::filesystem::create_directory_symlink(elsewhere, bundle / "Contents" / "MacOS", code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value());
        EXPECT_NE(error.find("outside the bundle"), std::string::npos) << error;
    }

    TEST(MacosBundle, DoesNotReadANonRegularInfoPlist)
    {
        const sogen::test::temp_directory dir{"bundle-devzero"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget", false);

        std::error_code code{};
        std::filesystem::create_symlink("/dev/zero", bundle / "Contents" / "Info.plist", code);
        ASSERT_FALSE(code) << code.message();

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->executable_name, "Widget");
    }

    // Opening a FIFO with no writer blocks forever, so the plist must be rejected by stat, never opened.
    TEST(MacosBundle, DoesNotOpenAFifoInfoPlist)
    {
        const sogen::test::temp_directory dir{"bundle-fifo"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget", false);

        const auto info_plist_path = bundle / "Contents" / "Info.plist";
        ASSERT_EQ(::mkfifo(info_plist_path.string().c_str(), 0600), 0);

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->executable_name, "Widget");
    }

    TEST(MacosBundle, FallsBackWhenTheInfoPlistIsLargerThanTheParserAccepts)
    {
        const sogen::test::temp_directory dir{"bundle-hugeplist"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget", false);

        std::string huge = info_plist("Impostor");
        huge.append(sogen::MAX_PLIST_SIZE, ' ');
        write_text(bundle / "Contents" / "Info.plist", huge);

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle, error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->executable_name, "Widget");
    }

    TEST(MacosBundle, RejectsAnExecutableNameContainingABackslash)
    {
        const sogen::test::temp_directory dir{"bundle-backslash"};
        const auto bundle = make_bundle(dir.path(), "Widget", "Widget");
        write_text(bundle / "Contents" / "Info.plist", info_plist("..\\..\\Widget"));

        std::string error{};
        EXPECT_FALSE(sogen::resolve_app_bundle(bundle, error).has_value());
        EXPECT_NE(error.find("CFBundleExecutable"), std::string::npos) << error;
    }

    TEST(MacosBundle, ResolvesABundlePathWithATrailingSeparator)
    {
        const sogen::test::temp_directory dir{"bundle-trailing"};
        const auto bundle = make_bundle(dir.path(), "Widget", "WidgetBinary");

        EXPECT_TRUE(sogen::is_app_bundle_path(bundle.string() + "/"));

        std::string error{};
        const auto resolved = sogen::resolve_app_bundle(bundle.string() + "/", error);
        ASSERT_TRUE(resolved.has_value()) << error;
        EXPECT_EQ(resolved->bundle_name, "Widget.app");
        EXPECT_EQ(resolved->executable_name, "WidgetBinary");
    }
}
