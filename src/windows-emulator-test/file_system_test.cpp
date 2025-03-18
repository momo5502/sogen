#include <gtest/gtest.h>

#include <file_system.hpp>

namespace test
{
    TEST(FileSystemTest, PathTraversalIsNotPossible)
    {
        const auto current_dir = std::filesystem::current_path();

        const file_system fs{current_dir};

        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"..", u"..", u"..", u"..", u"a.txt"})));
        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"b", u"..", u"..", u"b", u"..", u"a.txt"})));
        EXPECT_EQ(current_dir / "a", fs.translate(windows_path('a', {u"..", u"b"})));
    }
}
