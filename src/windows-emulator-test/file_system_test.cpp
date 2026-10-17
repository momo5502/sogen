#include <gtest/gtest.h>

#include <emulator_utils.hpp>
#include <file_system.hpp>
#include <filesystem/guest_file.hpp>

#include <fstream>
#include <random>

namespace sogen::test
{
    namespace
    {
        void write_host_file(const std::filesystem::path& path, const std::string_view contents)
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        }

        std::string read_host_file(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        std::string read_guest_file(const guest_file& file, const size_t size)
        {
            std::string contents(size, '\0');
            contents.resize(file.read(std::as_writable_bytes(std::span(contents))));
            return contents;
        }

        class FileSystemTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root = std::filesystem::temp_directory_path() /
                             ("sogen-file-system-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(this->root / "c" / "windows");
                std::filesystem::create_directories(this->root / "a");
                std::filesystem::create_directories(this->root / "mapped" / "deep");
                std::filesystem::create_directories(this->root / "other");
                this->root = std::filesystem::canonical(this->root);

                write_host_file(this->root / "c" / "x.txt", "root");
                write_host_file(this->root / "mapped" / "x.txt", "mapped");
                write_host_file(this->root / "other" / "y.txt", "other");
                write_host_file(this->root / "single.txt", "single");
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            std::filesystem::path root{};
        };
    }

    TEST_F(FileSystemTest, ParentComponentsResolveInGuestSpace)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped");

        EXPECT_EQ(this->root / "a" / "a.txt", fs.translate(windows_path{'a', {u"..", u"..", u"..", u"a.txt"}}));
        EXPECT_EQ(this->root / "a" / "a.txt", fs.translate(windows_path{'a', {u"b", u"..", u"..", u"b", u"..", u"a.txt"}}));
        EXPECT_EQ(this->root / "c" / "x.txt", fs.translate(windows_path{'c', {u"mnt", u"..", u"x.txt"}}));
        EXPECT_EQ(this->root / "c" / "x.txt", fs.translate(windows_path{'c', {u"mnt", u"..", u"..", u"x.txt"}}));
    }

    TEST_F(FileSystemTest, DeepestMountWins)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped");
        fs.map(windows_path{'c', {u"mnt", u"deep"}}, this->root / "other");
        fs.map(windows_path{'c', {u"mnt", u"exact.txt"}}, this->root / "single.txt");

        EXPECT_EQ(this->root / "mapped" / "sub" / "a.txt", fs.translate(windows_path{'c', {u"mnt", u"sub", u"a.txt"}}));
        EXPECT_EQ(this->root / "other" / "y.txt", fs.translate(windows_path{'c', {u"mnt", u"deep", u"y.txt"}}));
        EXPECT_EQ(this->root / "single.txt", fs.translate(windows_path{'c', {u"mnt", u"exact.txt"}}));

        const auto result = fs.open(windows_path{'c', {u"mnt", u"exact.txt"}}, u"rb");
        ASSERT_TRUE(result.file);
        EXPECT_EQ(6, result.file->size());
    }

    TEST_F(FileSystemTest, UnknownDrivesDoNotResolve)
    {
        file_system fs{this->root};

        EXPECT_TRUE(fs.translate(windows_path{'z', {u"x.txt"}}).empty());
        EXPECT_FALSE(fs.stat(windows_path{'z', {}}));

        const auto result = fs.open(windows_path{'z', {u"x.txt"}}, u"wb");
        EXPECT_FALSE(result.file);
        EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, result.status);
        EXPECT_FALSE(std::filesystem::exists(this->root / "z"));
    }

    TEST_F(FileSystemTest, DrivesComeFromMounts)
    {
        file_system fs{this->root};
        EXPECT_EQ((std::set<char>{'a', 'c'}), fs.list_drives());

        fs.map(windows_path{'d', {}}, this->root / "other");
        EXPECT_EQ((std::set<char>{'a', 'c', 'd'}), fs.list_drives());
        EXPECT_EQ(this->root / "other" / "y.txt", fs.translate(windows_path{'d', {u"y.txt"}}));
    }

    TEST_F(FileSystemTest, ListingIncludesChildMounts)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped");
        fs.map(windows_path{'c', {u"windows"}}, this->root / "single.txt");
        fs.map(windows_path{'c', {u"missing"}}, this->root / "does-not-exist");

        std::map<std::u16string, bool> entries{};
        for (const auto& entry : fs.list_directory(windows_path{'c', {}}))
        {
            EXPECT_FALSE(entries.contains(entry.name));
            entries[entry.name] = entry.is_directory;
        }

        EXPECT_EQ((std::map<std::u16string, bool>{{u"mnt", true}, {u"windows", false}, {u"x.txt", false}}), entries);
    }

    TEST_F(FileSystemTest, RenameAcrossMountsCopiesFile)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped");

        EXPECT_EQ(STATUS_SUCCESS, fs.rename(windows_path{'c', {u"mnt", u"x.txt"}}, windows_path{'c', {u"moved.txt"}}));
        EXPECT_FALSE(std::filesystem::exists(this->root / "mapped" / "x.txt"));
        EXPECT_EQ("mapped", read_host_file(this->root / "c" / "moved.txt"));

        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, fs.rename(windows_path{'c', {u"moved.txt"}}, windows_path{'a', {u"moved.txt"}}));
        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, fs.rename(windows_path{'c', {u"mnt", u"deep"}}, windows_path{'c', {u"deep"}}));
    }

    TEST_F(FileSystemTest, DirectoryMountPointsCannotBeRemovedOrRenamed)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped");

        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.remove(windows_path{'c', {u"mnt"}}));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.remove(windows_path{'c', {}}));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.rename(windows_path{'c', {u"mnt"}}, windows_path{'c', {u"renamed"}}));
        EXPECT_TRUE(std::filesystem::exists(this->root / "mapped" / "x.txt"));
    }

    TEST_F(FileSystemTest, FileMountsCanBeRemovedAndRenamed)
    {
        file_system fs{this->root};
        fs.map(windows_path{'c', {u"single.txt"}}, this->root / "single.txt");
        fs.map(windows_path{'c', {u"other.txt"}}, this->root / "other" / "y.txt");

        EXPECT_EQ(STATUS_SUCCESS, fs.rename(windows_path{'c', {u"x.txt"}}, windows_path{'c', {u"single.txt"}}));
        EXPECT_FALSE(std::filesystem::exists(this->root / "c" / "x.txt"));
        EXPECT_EQ("root", read_host_file(this->root / "single.txt"));

        EXPECT_EQ(STATUS_SUCCESS, fs.rename(windows_path{'c', {u"other.txt"}}, windows_path{'c', {u"moved.txt"}}));
        EXPECT_FALSE(std::filesystem::exists(this->root / "other" / "y.txt"));
        EXPECT_EQ("other", read_host_file(this->root / "c" / "moved.txt"));

        EXPECT_EQ(STATUS_SUCCESS, fs.remove(windows_path{'c', {u"single.txt"}}));
        EXPECT_FALSE(std::filesystem::exists(this->root / "single.txt"));
    }

    TEST_F(FileSystemTest, ReadOnlyMountsRejectWrites)
    {
        file_system fs{this->root, filesystem_mode::read_only};
        fs.map(windows_path{'c', {u"mnt"}}, this->root / "mapped", filesystem_mode::read_write);

        const windows_path existing{'c', {u"x.txt"}};
        const windows_path missing{'c', {u"new.txt"}};

        EXPECT_FALSE(fs.is_writable(existing));
        EXPECT_TRUE(fs.open(existing, u"rb").file);
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.open(existing, u"r+b").status);
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.open(existing, u"ab").status);
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.open(missing, u"wb").status);
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.create_directory(windows_path{'c', {u"dir"}}));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.remove(existing));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.rename(existing, missing));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.rename(windows_path{'c', {u"mnt", u"x.txt"}}, missing));
        EXPECT_EQ(STATUS_ACCESS_DENIED, fs.rename(existing, windows_path{'c', {u"mnt", u"moved.txt"}}));

        EXPECT_FALSE(std::filesystem::exists(this->root / "c" / "new.txt"));
        EXPECT_FALSE(std::filesystem::exists(this->root / "c" / "dir"));
        EXPECT_EQ("root", read_host_file(this->root / "c" / "x.txt"));
        EXPECT_EQ("mapped", read_host_file(this->root / "mapped" / "x.txt"));

        const windows_path writable{'c', {u"mnt", u"new.txt"}};
        EXPECT_TRUE(fs.is_writable(writable));
        EXPECT_TRUE(fs.open(writable, u"wb").file);
        EXPECT_EQ(STATUS_SUCCESS, fs.create_directory(windows_path{'c', {u"mnt", u"dir"}}));
        EXPECT_EQ(STATUS_SUCCESS, fs.remove(writable));
    }

    TEST_F(FileSystemTest, ExplicitMountReplacesRootDrive)
    {
        file_system fs{this->root, filesystem_mode::read_only};
        fs.map(windows_path{'c', {}}, this->root / "mapped", filesystem_mode::read_write);

        EXPECT_EQ(this->root / "mapped" / "x.txt", fs.translate(windows_path{'c', {u"x.txt"}}));
        EXPECT_TRUE(fs.is_writable(windows_path{'c', {u"x.txt"}}));
        EXPECT_FALSE(fs.is_writable(windows_path{'a', {u"x.txt"}}));
    }

    TEST_F(FileSystemTest, HandlesReportWriteAccess)
    {
        file_system fs{this->root};
        const windows_path path{'c', {u"x.txt"}};

        const guest_file reader{fs, path, u"rb", fs.open(path, u"rb").file};
        const guest_file writer{fs, path, u"r+b", fs.open(path, u"r+b").file};
        const guest_file directory{fs, windows_path{'c', {}}};

        EXPECT_FALSE(reader.is_writable());
        EXPECT_TRUE(writer.is_writable());
        EXPECT_FALSE(directory.is_writable());
    }

    TEST_F(FileSystemTest, DeferredOperationsRunOnClose)
    {
        file_system fs{this->root};
        const windows_path source{'c', {u"x.txt"}};
        const windows_path target{'c', {u"renamed.txt"}};

        {
            guest_file file{fs, source, u"rb", fs.open(source, u"rb").file};
            file.defer_rename(target);
        }

        EXPECT_FALSE(std::filesystem::exists(this->root / "c" / "x.txt"));
        EXPECT_EQ("root", read_host_file(this->root / "c" / "renamed.txt"));

        {
            guest_file file{fs, target, u"rb", fs.open(target, u"rb").file};
            file.set_delete_on_close(true);
        }

        EXPECT_FALSE(std::filesystem::exists(this->root / "c" / "renamed.txt"));
    }

    TEST_F(FileSystemTest, SnapshotRestoreKeepsWrittenContents)
    {
        file_system fs{this->root};
        const windows_path path{'c', {u"new.bin"}};
        constexpr std::string_view contents = "snapshot";

        guest_file file{fs, path, u"w+b", fs.open(path, u"w+b").file};
        ASSERT_TRUE(file);
        ASSERT_EQ(contents.size(), file.write(std::as_bytes(std::span(contents))));
        ASSERT_TRUE(file.flush());
        ASSERT_TRUE(file.seek_to(4));

        utils::buffer_serializer serializer{};
        serializer.write(file);

        utils::buffer_deserializer deserializer{serializer};
        deserializer.register_factory<file_system_wrapper>([&] { return file_system_wrapper{fs}; });

        guest_file restored{};
        deserializer.read(restored);

        ASSERT_TRUE(restored);
        EXPECT_EQ(4, restored.tell());
        EXPECT_EQ("shot", read_guest_file(restored, 16));
        EXPECT_EQ(contents, read_host_file(this->root / "c" / "new.bin"));
    }
} // namespace sogen::test
