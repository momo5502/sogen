#include <gtest/gtest.h>

#include <file_system.hpp>

namespace sogen::test
{
    TEST(FileSystemTest, PathTraversalIsNotPossible)
    {
        const auto current_dir = std::filesystem::current_path();

        const file_system fs{current_dir};

        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"..", u"..", u"..", u"..", u"a.txt"})));
        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"b", u"..", u"..", u"b", u"..", u"a.txt"})));
        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"..", u"b"})));
    }

    TEST(FileSystemTest, DirectoryMappingMountsSubtree)
    {
        const auto current_dir = std::filesystem::current_path();

        file_system fs{current_dir};
        fs.map(windows_path('c', {u"mnt"}), current_dir);

        // A file under the mounted directory resolves to the same file under the host directory.
        EXPECT_EQ(current_dir / "sub" / "a.txt", fs.translate(windows_path('c', {u"mnt", u"sub", u"a.txt"})));

        // ".." cannot escape the mount; it is confined to the mounted directory.
        EXPECT_EQ(current_dir, fs.translate(windows_path('c', {u"mnt", u"..", u"..", u"etc"})));

        // The deepest matching mount wins.
        fs.map(windows_path('c', {u"mnt", u"deep"}), current_dir / "other");
        EXPECT_EQ(current_dir / "other" / "x.txt", fs.translate(windows_path('c', {u"mnt", u"deep", u"x.txt"})));

        // An exact file mapping still takes precedence over the directory mount.
        fs.map(windows_path('c', {u"mnt", u"exact.txt"}), current_dir / "elsewhere.txt");
        EXPECT_EQ(current_dir / "elsewhere.txt", fs.translate(windows_path('c', {u"mnt", u"exact.txt"})));
    }
} // namespace sogen::test
