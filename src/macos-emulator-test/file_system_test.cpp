#include <gtest/gtest.h>

#include <guest/guest_file_system.hpp>

#include "fixture_utils.hpp"

namespace
{
    TEST(GuestFileSystem, NormalizesGuestPaths)
    {
        EXPECT_EQ(sogen::guest_file_system::normalize_guest_path_string("/usr/lib/../bin/./sh"), "/usr/bin/sh");
        EXPECT_EQ(sogen::guest_file_system::normalize_guest_path_string("///usr//lib//"), "/usr/lib");
        EXPECT_EQ(sogen::guest_file_system::normalize_guest_path_string(""), "/");
    }

    TEST(GuestFileSystem, TranslatesAbsoluteGuestPathsUnderTheRoot)
    {
        const sogen::guest_file_system fs{std::filesystem::path{"/emulation/root"}};

        EXPECT_EQ(fs.translate("/usr/lib/dyld").generic_string(), "/emulation/root/usr/lib/dyld");
        EXPECT_EQ(fs.translate("/").generic_string(), "/emulation/root/");
        EXPECT_EQ(fs.translate("").generic_string(), "/emulation/root");
    }

    TEST(GuestFileSystem, EscapeAttemptsCannotLeaveTheRoot)
    {
        const sogen::guest_file_system fs{std::filesystem::path{"/emulation/root"}};

        EXPECT_EQ(fs.translate("/../../etc/passwd").generic_string(), "/emulation/root/etc/passwd");
    }

    TEST(GuestFileSystem, ResolvesRelativePathsAgainstTheWorkingDirectory)
    {
        const sogen::guest_file_system fs{std::filesystem::path{"/emulation/root"}};

        EXPECT_EQ(fs.translate_guest_relative_to("/usr/lib", "dyld").generic_string(), "/emulation/root/usr/lib/dyld");
    }

    TEST(GuestFileSystem, ExplicitMappingsWinOverTheRoot)
    {
        sogen::guest_file_system fs{std::filesystem::path{"/emulation/root"}};
        fs.add_path_mapping("/System/Library", "/host/system-library", true);

        EXPECT_EQ(fs.translate("/System/Library/CoreFoundation").generic_string(), "/host/system-library/CoreFoundation");
        EXPECT_TRUE(fs.is_read_only_guest_path("/System/Library/CoreFoundation"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/usr/lib/dyld"));
    }

    TEST(GuestFileSystem, ReadOnlyMappingsCoverTheHostPathAGuestNamesDirectly)
    {
        sogen::guest_file_system fs{};
        fs.add_path_mapping("/Applications/Widget.app", "/host/Widget.app", true);

        EXPECT_EQ(fs.translate("/host/Widget.app/Contents/MacOS/Widget").generic_string(), "/host/Widget.app/Contents/MacOS/Widget");
        EXPECT_TRUE(fs.is_read_only_guest_path("/host/Widget.app/Contents/MacOS/Widget"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/host/Widget.app.backup/secret.txt"));
    }

    TEST(GuestFileSystem, ReadOnlyMappingsSurviveAPassthroughPrefix)
    {
        sogen::guest_file_system fs{std::filesystem::path{"/emulation/root"}};
        fs.add_path_mapping("/Applications/Widget.app", "/host/Widget.app", true);
        fs.add_passthrough_prefix("/host/Widget.app/Contents/MacOS");

        EXPECT_EQ(fs.translate("/host/Widget.app/Contents/MacOS/Widget").generic_string(), "/host/Widget.app/Contents/MacOS/Widget");
        EXPECT_TRUE(fs.is_read_only_guest_path("/host/Widget.app/Contents/MacOS/Widget"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/usr/lib/dyld"));
    }

    TEST(GuestFileSystem, ReadOnlyMappingsCoverASymlinkedAliasOfTheHostRoot)
    {
        const sogen::test::temp_directory dir{"fs-readonly-alias"};
        const auto real_root = dir.path() / "real";
        std::filesystem::create_directories(real_root / "Contents");

        const auto alias_root = dir.path() / "alias";
        std::error_code error{};
        std::filesystem::create_directory_symlink(real_root, alias_root, error);
        ASSERT_FALSE(error) << error.message();

        sogen::guest_file_system fs{};
        fs.add_path_mapping("/Applications/Widget.app", real_root, true);

        EXPECT_TRUE(fs.is_read_only_guest_path((alias_root / "Contents" / "data.txt").generic_string()));
        EXPECT_FALSE(fs.is_read_only_guest_path((dir.path() / "elsewhere" / "data.txt").generic_string()));
    }

    // A root staged by src/tools/make-macos-root.sh is a tree of symlinks into the running system, so
    // clamping the guest path into the root leaves the host open() free to follow one outward. These pin
    // the measured escapes: before the containment rule the guest created and deleted files in the
    // analyst's real /Applications and /Library/Caches, and in the directory its own sample was staged in.
    TEST(GuestFileSystem, WritesThatResolveOutsideTheRootAreRefused)
    {
        const sogen::test::temp_directory dir{"fs-escape"};
        const auto root = dir.path() / "root";
        const auto outside = dir.path() / "outside";
        std::filesystem::create_directories(root);
        std::filesystem::create_directories(outside);

        std::error_code error{};
        std::filesystem::create_directory_symlink(outside, root / "Applications", error);
        ASSERT_FALSE(error) << error.message();

        const sogen::guest_file_system fs{root};

        EXPECT_EQ(fs.translate("/Applications/victim.txt"), root / "Applications" / "victim.txt");
        EXPECT_TRUE(fs.is_read_only_guest_path("/Applications/victim.txt"));
        EXPECT_TRUE(fs.is_read_only_guest_path("/Applications/nested/victim.txt"));
    }

    TEST(GuestFileSystem, WritesThatStayInsideTheRootAreAllowed)
    {
        const sogen::test::temp_directory dir{"fs-scratch"};
        const auto root = dir.path() / "root";
        std::filesystem::create_directories(root / "private" / "tmp");

        const sogen::guest_file_system fs{root};

        EXPECT_FALSE(fs.is_read_only_guest_path("/private/tmp/scratch.txt"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/System/Library/staged.txt"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/does/not/exist/yet.txt"));
    }

    // The emulator hands the directory of the sample to add_passthrough_prefix so the loader can open the
    // executable and its sibling dylibs by host path. That is a read grant; a sample staged in the
    // analyst's ~/Downloads must not be able to drop a file beside itself or delete its neighbours.
    TEST(GuestFileSystem, PassthroughPrefixesDoNotGrantWrites)
    {
        const sogen::test::temp_directory dir{"fs-passthrough"};
        const auto root = dir.path() / "root";
        const auto staging = dir.path() / "downloads";
        std::filesystem::create_directories(root);
        std::filesystem::create_directories(staging);

        sogen::guest_file_system fs{root};
        fs.add_passthrough_prefix(staging);

        EXPECT_EQ(fs.translate((staging / "sample").generic_string()), staging / "sample");
        EXPECT_TRUE(fs.is_read_only_guest_path((staging / "dropped.txt").generic_string()));
    }

    TEST(GuestFileSystem, ExplicitWritableMappingsSurviveTheRootClamp)
    {
        const sogen::test::temp_directory dir{"fs-writable-mapping"};
        const auto root = dir.path() / "root";
        const auto shared = dir.path() / "shared";
        const auto bundle = dir.path() / "bundle";
        std::filesystem::create_directories(root);
        std::filesystem::create_directories(shared);
        std::filesystem::create_directories(bundle);

        sogen::guest_file_system fs{root};
        fs.add_path_mapping("/shared", shared, false);
        fs.add_path_mapping("/bundle", bundle, true);

        EXPECT_FALSE(fs.is_read_only_guest_path("/shared/report.txt"));
        EXPECT_TRUE(fs.is_read_only_guest_path("/bundle/report.txt"));
    }

    TEST(GuestFileSystem, CharacterDevicesStayWritableUnderARoot)
    {
        const sogen::test::temp_directory dir{"fs-devices"};
        const auto root = dir.path() / "root";
        std::filesystem::create_directories(root);

        const sogen::guest_file_system fs{root};

        EXPECT_FALSE(fs.is_read_only_guest_path("/dev/null"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/dev/zero"));
        EXPECT_FALSE(fs.is_read_only_guest_path("/dev/urandom"));
    }

    TEST(GuestFileSystem, WithoutARootNothingIsClamped)
    {
        const sogen::guest_file_system fs{};

        EXPECT_FALSE(fs.is_read_only_guest_path("/anywhere/at/all.txt"));
    }
}
