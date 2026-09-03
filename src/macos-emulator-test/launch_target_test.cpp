#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <guest/guest_file_system.hpp>
#include <macos_launch_target.hpp>

#include "fixture_utils.hpp"

namespace
{
    void write_text(const std::filesystem::path& path, const std::string& text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << text;
    }

    std::filesystem::path make_bundle(const std::filesystem::path& parent, const std::string& name)
    {
        const auto bundle = parent / (name + ".app");
        write_text(bundle / "Contents" / "Info.plist", R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>)" + name + R"(</string>
	<key>CFBundleIdentifier</key>
	<string>dev.sogen.)" + name + R"(</string>
</dict>
</plist>
)");
        write_text(bundle / "Contents" / "MacOS" / name, "\xcf\xfa\xed\xfe payload");
        write_text(bundle / "Contents" / "Resources" / "data.txt", "resource");
        return bundle;
    }

    TEST(MacosLaunchTarget, ResolvesAnAppBundleToGuestPaths)
    {
        const sogen::test::temp_directory dir{"launch-bundle"};
        const auto bundle = make_bundle(dir.path(), "Widget");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;
        EXPECT_EQ(target.kind, sogen::macos_input_kind::app_bundle);
        EXPECT_EQ(target.guest_executable, "/Applications/Widget.app/Contents/MacOS/Widget");
        EXPECT_EQ(target.working_directory, "/");
        EXPECT_EQ(target.bundle_identifier, "dev.sogen.Widget");
        ASSERT_TRUE(target.bundle.has_value());
        EXPECT_EQ(target.bundle->guest_root, "/Applications/Widget.app");
    }

    TEST(MacosLaunchTarget, MapsTheWholeBundleReadOnly)
    {
        const sogen::test::temp_directory dir{"launch-mapping"};
        const auto bundle = make_bundle(dir.path(), "Widget");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        sogen::guest_file_system fs{};
        sogen::apply_macos_launch_target(target, fs);

        EXPECT_EQ(fs.translate(target.guest_executable), target.host_executable);
        EXPECT_EQ(fs.translate("/Applications/Widget.app/Contents/Resources/data.txt"),
                  std::filesystem::weakly_canonical(bundle) / "Contents" / "Resources" / "data.txt");
        EXPECT_TRUE(fs.is_read_only_guest_path("/Applications/Widget.app/Contents/Info.plist"));
        EXPECT_TRUE(fs.is_read_only_guest_path((bundle / "Contents" / "Info.plist").generic_string()));
    }

    TEST(MacosLaunchTarget, KeepsBundleRelativeTraversalInsideTheBundle)
    {
        const sogen::test::temp_directory dir{"launch-escape"};
        const auto bundle = make_bundle(dir.path(), "Widget");
        write_text(dir.path() / "outside.txt", "secret");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        sogen::guest_file_system fs{};
        sogen::apply_macos_launch_target(target, fs);

        const auto escaped = fs.translate("/Applications/Widget.app/../outside.txt");
        EXPECT_NE(escaped, (dir.path() / "outside.txt").lexically_normal());
    }

    // KeepsBundleRelativeTraversalInsideTheBundle holds vacuously: normalize_guest_path_string collapses
    // ".." before any mapping is consulted, so it passes even with apply_macos_launch_target gutted. The
    // escape that a path mapping can actually produce is a sibling whose name merely starts with the
    // bundle's, which only a component-wise prefix test rejects.
    TEST(MacosLaunchTarget, ASiblingSharingTheBundleNamePrefixIsNotMapped)
    {
        const sogen::test::temp_directory dir{"launch-sibling"};
        const auto bundle = make_bundle(dir.path(), "Widget");
        write_text(dir.path() / "Widget.app.backup" / "secret.txt", "secret");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;

        sogen::guest_file_system fs{};
        sogen::apply_macos_launch_target(target, fs);

        EXPECT_EQ(fs.translate("/Applications/Widget.app.backup/secret.txt"),
                  std::filesystem::path{"/Applications/Widget.app.backup/secret.txt"});
        EXPECT_NE(fs.translate("/Applications/Widget.app.backup/secret.txt"),
                  (dir.path() / "Widget.app.backup" / "secret.txt").lexically_normal());
        EXPECT_FALSE(fs.is_read_only_guest_path("/Applications/Widget.app.backup/secret.txt"));
    }

    TEST(MacosLaunchTarget, RefusesABundleWhoseDeclaredExecutableEscapes)
    {
        const sogen::test::temp_directory dir{"launch-plist-escape"};
        const auto bundle = dir.path() / "Widget.app";
        write_text(bundle / "Contents" / "Info.plist", R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>../../../../../../../../../../../../bin/sh</string>
</dict>
</plist>
)");
        write_text(bundle / "Contents" / "MacOS" / "Widget", "\xcf\xfa\xed\xfe payload");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        EXPECT_FALSE(target.runnable());
        EXPECT_TRUE(target.guest_executable.empty());
        EXPECT_FALSE(target.bundle.has_value());
        EXPECT_NE(target.diagnostic.find("single path component"), std::string::npos) << target.diagnostic;
    }

    // Reaching a bundle through its inner executable must not become a way to launch a file that
    // resolve_app_bundle refuses: the bundle error wins over a plain, unmapped launch.
    TEST(MacosLaunchTarget, RefusesAnInnerExecutableWhoseBundleDoesNotResolve)
    {
        const sogen::test::temp_directory dir{"launch-inner-broken"};
        const auto bundle = dir.path() / "Widget.app";
        write_text(bundle / "Contents" / "Info.plist", R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>Ghost</string>
</dict>
</plist>
)");
        write_text(bundle / "Contents" / "MacOS" / "Real", "\xcf\xfa\xed\xfe payload");

        const auto target = sogen::resolve_macos_launch_target(bundle / "Contents" / "MacOS" / "Real");
        EXPECT_FALSE(target.runnable());
        EXPECT_EQ(target.kind, sogen::macos_input_kind::app_bundle);
        EXPECT_TRUE(target.guest_executable.empty());
        EXPECT_NE(target.diagnostic.find("Contents/MacOS/Ghost"), std::string::npos) << target.diagnostic;
    }

    TEST(MacosLaunchTarget, TreatsAnExecutableInsideABundleAsABundleLaunch)
    {
        const sogen::test::temp_directory dir{"launch-inner"};
        const auto bundle = make_bundle(dir.path(), "Widget");

        const auto target = sogen::resolve_macos_launch_target(bundle / "Contents" / "MacOS" / "Widget");
        ASSERT_TRUE(target.runnable()) << target.diagnostic;
        EXPECT_EQ(target.kind, sogen::macos_input_kind::app_bundle);
        EXPECT_EQ(target.guest_executable, "/Applications/Widget.app/Contents/MacOS/Widget");
    }

    TEST(MacosLaunchTarget, ResolvesAPlainMachOWithoutAMapping)
    {
        const sogen::test::temp_directory dir{"launch-plain"};
        const auto path = dir.path() / "program";
        write_text(path, "\xcf\xfa\xed\xfe payload");

        const auto target = sogen::resolve_macos_launch_target(path);
        ASSERT_TRUE(target.runnable()) << target.diagnostic;
        EXPECT_EQ(target.kind, sogen::macos_input_kind::mach_o);
        EXPECT_FALSE(target.bundle.has_value());
        EXPECT_EQ(target.host_executable, path.lexically_normal());
        EXPECT_EQ(target.working_directory, dir.path().lexically_normal().generic_string());
    }

    TEST(MacosLaunchTarget, RefusesADiskImageWithADiagnostic)
    {
        const sogen::test::temp_directory dir{"launch-dmg"};
        const auto path = dir.path() / "sample.dmg";
        write_text(path, std::string(2048, 'x'));

        const auto target = sogen::resolve_macos_launch_target(path);
        EXPECT_FALSE(target.runnable());
        EXPECT_EQ(target.kind, sogen::macos_input_kind::disk_image);
        EXPECT_NE(target.diagnostic.find("hdiutil attach"), std::string::npos);
    }

    TEST(MacosLaunchTarget, RefusesABrokenBundleWithTheBundleError)
    {
        const sogen::test::temp_directory dir{"launch-broken"};
        const auto bundle = dir.path() / "Widget.app";
        std::filesystem::create_directories(bundle / "Contents" / "MacOS");

        const auto target = sogen::resolve_macos_launch_target(bundle);
        EXPECT_FALSE(target.runnable());
        EXPECT_NE(target.diagnostic.find("Contents/MacOS"), std::string::npos);
    }

    TEST(MacosLaunchTarget, RefusesAMissingPath)
    {
        const sogen::test::temp_directory dir{"launch-missing"};
        const auto target = sogen::resolve_macos_launch_target(dir.path() / "absent");
        EXPECT_FALSE(target.runnable());
        EXPECT_EQ(target.kind, sogen::macos_input_kind::missing);
    }
}
