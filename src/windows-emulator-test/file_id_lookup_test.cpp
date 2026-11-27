#include "std_include.hpp"

namespace sogen::test
{
    namespace
    {
        class FileIdLookupTest : public testing::Test
        {
          protected:
            std::filesystem::path root = std::filesystem::temp_directory_path() / ("sogen-file-id-lookup-test-" + std::to_string(getpid()) +
                                                                                   "-" + std::to_string(std::random_device{}()));

            void SetUp() override
            {
                ASSERT_TRUE(std::filesystem::create_directories(this->root / "c" / "sub"));
                ASSERT_TRUE(std::filesystem::create_directories(this->root / "shadow"));
                this->root = std::filesystem::canonical(this->root);
                std::ofstream(this->root / "c" / "sub" / "file.txt") << "data";
                std::ofstream(this->root / "c" / "top.txt") << "data";
            }

            void TearDown() override
            {
                std::error_code error{};
                std::filesystem::remove_all(this->root, error);
            }

            static uint64_t id_of(const file_system& fs, const windows_path& path)
            {
                const auto stat = fs.stat(path);
                EXPECT_TRUE(stat);
                return stat ? stat->st_ino : 0;
            }
        };
    }

    TEST_F(FileIdLookupTest, FindsOnlyThePathsTheGuestCanReach)
    {
        {
            SCOPED_TRACE("every entry of a drive, including the root");
            file_system fs{this->root, filesystem_mode::read_write};

            const std::array<windows_path, 4> entries{
                windows_path{'c', {}},
                windows_path{'c', {u"sub"}},
                windows_path{'c', {u"sub", u"file.txt"}},
                windows_path{'c', {u"top.txt"}},
            };

            for (const auto& entry : entries)
            {
                SCOPED_TRACE(entry.string());
                EXPECT_EQ(entry, fs.find_by_file_id('c', id_of(fs, entry)));
                EXPECT_EQ(entry, fs.find_by_file_id('C', id_of(fs, entry)));
            }

            EXPECT_FALSE(fs.find_by_file_id('d', id_of(fs, entries.back())));
            EXPECT_FALSE(fs.find_by_file_id('c', 0x0000123456789abcULL));
        }

        {
            SCOPED_TRACE("a path hidden by a more specific mount");
            file_system fs{this->root, filesystem_mode::read_write};
            const auto id = id_of(fs, windows_path{'c', {u"sub", u"file.txt"}});

            fs.map(windows_path{'c', {u"sub"}}, this->root / "shadow");

            EXPECT_FALSE(fs.find_by_file_id('c', id));
        }

        {
            SCOPED_TRACE("a file removed from a copy-on-write drive");
            file_system fs{this->root, filesystem_mode::copy_on_write, this->root / "layer"};
            const windows_path file{'c', {u"top.txt"}};
            const auto id = id_of(fs, file);
            ASSERT_EQ(file, fs.find_by_file_id('c', id));

            ASSERT_EQ(STATUS_SUCCESS, fs.remove(file));

            EXPECT_FALSE(fs.find_by_file_id('c', id));
        }
    }
}
