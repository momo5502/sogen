#include <gtest/gtest.h>

#include <emulator_utils.hpp>
#include <file_system.hpp>
#include <filesystem/host_directory_provider.hpp>
#include <filesystem/layered_provider.hpp>

#include <fstream>
#include <random>

namespace sogen::test
{
    namespace
    {
        void write_host_file(const std::filesystem::path& path, const std::string_view contents)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary);
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        }

        std::string read_host_file(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        std::string read_all(i_file& file)
        {
            std::string contents(static_cast<size_t>(file.size()), '\0');
            EXPECT_TRUE(file.seek(0, SEEK_SET));
            contents.resize(file.read(std::as_writable_bytes(std::span(contents))));
            return contents;
        }

        size_t write_text(i_file& file, const std::string_view text)
        {
            return file.write(std::as_bytes(std::span(text)));
        }

        windows_path guest(std::initializer_list<std::u16string> folders)
        {
            return windows_path{std::nullopt, std::list<std::u16string>(folders)};
        }

        std::set<std::u16string> names_in(const layered_provider& provider, const windows_path& directory)
        {
            std::set<std::u16string> names{};
            for (const auto& entry : provider.list(directory))
            {
                names.insert(entry.name);
            }

            return names;
        }

        class LayeredProviderTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root =
                    std::filesystem::temp_directory_path() /
                    ("sogen-layered-provider-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(this->root / "lower");
                this->root = std::filesystem::canonical(this->root);

                write_host_file(this->lower() / "file.txt", "lower contents");
                write_host_file(this->lower() / "dir" / "old.txt", "old");
                write_host_file(this->lower() / "dir" / "keep.txt", "keep");
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            std::filesystem::path lower() const
            {
                return this->root / "lower";
            }

            std::filesystem::path upper() const
            {
                return this->root / "upper";
            }

            std::filesystem::path state() const
            {
                return this->root / "state";
            }

            std::unique_ptr<layered_provider> make_provider() const
            {
                return std::make_unique<layered_provider>(std::make_unique<host_directory_provider>(this->lower()), this->upper(),
                                                          this->state());
            }

            std::filesystem::path root{};
        };
    }

    TEST_F(LayeredProviderTest, ReadsFallThroughToTheLower)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"rb");
        ASSERT_TRUE(result.file);
        EXPECT_EQ("lower contents", read_all(*result.file));
        EXPECT_FALSE(std::filesystem::exists(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, OpeningForWriteDoesNotCopyUntilTheFirstWrite)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        EXPECT_EQ("lower contents", read_all(*result.file));
        EXPECT_FALSE(std::filesystem::exists(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, FirstWriteCopiesUpAtTheCurrentPosition)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        ASSERT_TRUE(result.file->seek(6, SEEK_SET));
        EXPECT_EQ(3u, write_text(*result.file, "NEW"));
        ASSERT_TRUE(result.file->flush());

        EXPECT_EQ("lower NEWtents", read_host_file(this->upper() / "file.txt"));
        EXPECT_EQ("lower contents", read_host_file(this->lower() / "file.txt"));
        EXPECT_EQ(9, result.file->tell());
        EXPECT_EQ("lower NEWtents", read_all(*result.file));
    }

    TEST_F(LayeredProviderTest, TruncatingOpenCreatesAnEmptyUpperFile)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"wb");
        ASSERT_TRUE(result.file);
        EXPECT_EQ(0, result.file->size());
        EXPECT_EQ("", read_host_file(this->upper() / "file.txt"));
        EXPECT_EQ("lower contents", read_host_file(this->lower() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, ResizeCopiesUp)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        ASSERT_TRUE(result.file->resize(5));
        ASSERT_TRUE(result.file->flush());

        EXPECT_EQ("lower", read_host_file(this->upper() / "file.txt"));
        EXPECT_EQ("lower contents", read_host_file(this->lower() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, AppendCopiesUpAndAppends)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"ab");
        ASSERT_TRUE(result.file);
        EXPECT_EQ(4u, write_text(*result.file, "-end"));
        ASSERT_TRUE(result.file->flush());

        EXPECT_EQ("lower contents-end", read_host_file(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, HandlesOnTheSameLowerFileShareOneUpperFile)
    {
        const auto provider = this->make_provider();

        const auto first = provider->open(guest({u"file.txt"}), u"r+b");
        const auto second = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(first.file);
        ASSERT_TRUE(second.file);

        ASSERT_TRUE(second.file->seek(6, SEEK_SET));
        EXPECT_EQ(3u, write_text(*first.file, "AAA"));
        EXPECT_EQ(3u, write_text(*second.file, "BBB"));
        ASSERT_TRUE(first.file->flush());
        ASSERT_TRUE(second.file->flush());

        EXPECT_EQ("AAAer BBBtents", read_host_file(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, NewFilesLandInTheUpperWithTheirParents)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"dir", u"new.txt"}), u"wb");
        ASSERT_TRUE(result.file);
        EXPECT_EQ(3u, write_text(*result.file, "new"));
        ASSERT_TRUE(result.file->flush());

        EXPECT_EQ("new", read_host_file(this->upper() / "dir" / "new.txt"));
        EXPECT_FALSE(std::filesystem::exists(this->lower() / "dir" / "new.txt"));
        EXPECT_EQ((std::set<std::u16string>{u"old.txt", u"keep.txt", u"new.txt"}), names_in(*provider, guest({u"dir"})));

        EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, provider->open(guest({u"missing", u"new.txt"}), u"wb").status);
    }

    TEST_F(LayeredProviderTest, RemovedLowerFilesStayHiddenAcrossRestarts)
    {
        {
            const auto provider = this->make_provider();
            EXPECT_EQ(STATUS_SUCCESS, provider->remove(guest({u"file.txt"})));
            EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
            EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, provider->open(guest({u"file.txt"}), u"rb").status);
            EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, provider->remove(guest({u"file.txt"})));
        }

        const auto provider = this->make_provider();
        EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
        EXPECT_EQ(0u, names_in(*provider, {}).count(u"file.txt"));
        EXPECT_TRUE(std::filesystem::exists(this->lower() / "file.txt"));

        const auto recreated = provider->open(guest({u"file.txt"}), u"wb");
        ASSERT_TRUE(recreated.file);
        EXPECT_EQ(3u, write_text(*recreated.file, "new"));
        ASSERT_TRUE(recreated.file->flush());

        const auto reread = provider->open(guest({u"file.txt"}), u"rb");
        ASSERT_TRUE(reread.file);
        EXPECT_EQ("new", read_all(*reread.file));
    }

    TEST_F(LayeredProviderTest, DirectoriesNeedToBeEmptyToBeRemoved)
    {
        const auto provider = this->make_provider();

        EXPECT_EQ(STATUS_DIRECTORY_NOT_EMPTY, provider->remove(guest({u"dir"})));
        EXPECT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"old.txt"})));
        EXPECT_EQ(STATUS_DIRECTORY_NOT_EMPTY, provider->remove(guest({u"dir"})));
        EXPECT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"keep.txt"})));
        EXPECT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir"})));
        EXPECT_FALSE(provider->stat(guest({u"dir"})));
    }

    TEST_F(LayeredProviderTest, ARecreatedDirectoryDoesNotBringTheLowerContentBack)
    {
        {
            const auto provider = this->make_provider();
            ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"old.txt"})));
            ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"keep.txt"})));
            ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir"})));
            ASSERT_EQ(STATUS_SUCCESS, provider->create_directory(guest({u"dir"})));
            EXPECT_TRUE(names_in(*provider, guest({u"dir"})).empty());
        }

        const auto provider = this->make_provider();
        EXPECT_TRUE(names_in(*provider, guest({u"dir"})).empty());
        EXPECT_FALSE(provider->stat(guest({u"dir", u"old.txt"})));
        EXPECT_TRUE(std::filesystem::exists(this->lower() / "dir" / "old.txt"));
    }

    TEST_F(LayeredProviderTest, ExistingEntriesCannotBeCreatedAgain)
    {
        const auto provider = this->make_provider();

        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, provider->create_directory(guest({u"dir"})));
        EXPECT_EQ(STATUS_SUCCESS, provider->create_directory(guest({u"fresh"})));
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, provider->create_directory(guest({u"fresh"})));
        EXPECT_EQ(STATUS_OBJECT_PATH_NOT_FOUND, provider->create_directory(guest({u"missing", u"child"})));
    }

    TEST_F(LayeredProviderTest, ListingsMergeBothLayers)
    {
        const auto provider = this->make_provider();

        write_host_file(this->upper() / "only-upper.txt", "u");
        write_host_file(this->upper() / "file.txt", "shadow");

        EXPECT_EQ((std::set<std::u16string>{u"file.txt", u"dir", u"only-upper.txt"}), names_in(*provider, {}));

        const auto entries = provider->list({});
        EXPECT_EQ(3u, entries.size());

        const auto shadowed = provider->open(guest({u"file.txt"}), u"rb");
        ASSERT_TRUE(shadowed.file);
        EXPECT_EQ("shadow", read_all(*shadowed.file));
    }

    TEST_F(LayeredProviderTest, RenamingALowerFileMovesItIntoTheUpper)
    {
        const auto provider = this->make_provider();

        ASSERT_EQ(STATUS_SUCCESS, provider->rename(guest({u"file.txt"}), guest({u"dir", u"moved.txt"})));

        EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
        EXPECT_EQ("lower contents", read_host_file(this->upper() / "dir" / "moved.txt"));
        EXPECT_TRUE(std::filesystem::exists(this->lower() / "file.txt"));

        const auto restarted = this->make_provider();
        EXPECT_FALSE(restarted->stat(guest({u"file.txt"})));
        EXPECT_TRUE(restarted->stat(guest({u"dir", u"moved.txt"})));
    }

    TEST_F(LayeredProviderTest, AWriteThroughAnOldHandleDoesNotBringTheRenamedNameBack)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        ASSERT_EQ(STATUS_SUCCESS, provider->rename(guest({u"file.txt"}), guest({u"dir", u"moved.txt"})));

        EXPECT_EQ(0u, write_text(*result.file, "NEW"));
        EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
        EXPECT_FALSE(std::filesystem::exists(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, AWriteThroughAnOldHandleDoesNotModifyARecreatedFile)
    {
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"file.txt"})));

        const auto created = provider->open(guest({u"file.txt"}), u"wb");
        ASSERT_TRUE(created.file);
        ASSERT_EQ(3u, write_text(*created.file, "own"));
        ASSERT_TRUE(created.file->flush());

        EXPECT_EQ(0u, write_text(*result.file, "NEW"));
        EXPECT_EQ("own", read_host_file(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, AFailedRenameKeepsOpenHandlesUsable)
    {
        write_host_file(this->upper() / "taken.txt", "x");
        const auto provider = this->make_provider();

        const auto result = provider->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(result.file);
        EXPECT_NE(STATUS_SUCCESS, provider->rename(guest({u"file.txt"}), guest({u"taken.txt"})));

        EXPECT_EQ(3u, write_text(*result.file, "NEW"));
    }

    TEST_F(LayeredProviderTest, ARenameThatCannotHideTheOldNameLeavesNothingBehind)
    {
        std::filesystem::create_directories(this->state() / "journal");

        const auto provider = this->make_provider();
        EXPECT_NE(STATUS_SUCCESS, provider->rename(guest({u"file.txt"}), guest({u"dir", u"target.txt"})));

        EXPECT_TRUE(provider->stat(guest({u"file.txt"})));
        EXPECT_FALSE(provider->stat(guest({u"dir", u"target.txt"})));
        EXPECT_FALSE(std::filesystem::exists(this->upper() / "dir" / "target.txt"));
    }

    TEST_F(LayeredProviderTest, ALowerFileCannotReplaceAnExistingFileByRenaming)
    {
        write_host_file(this->upper() / "dir" / "target.txt", "keep me");

        const auto provider = this->make_provider();
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, provider->rename(guest({u"file.txt"}), guest({u"dir", u"old.txt"})));
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, provider->rename(guest({u"file.txt"}), guest({u"dir", u"target.txt"})));

        EXPECT_EQ("keep me", read_host_file(this->upper() / "dir" / "target.txt"));
        EXPECT_TRUE(provider->stat(guest({u"file.txt"})));
    }

    TEST_F(LayeredProviderTest, ATornJournalThatCannotBeCutIsReported)
    {
        write_host_file(this->state() / "journal", "H\tfile.txt\nH\tdir");
        std::filesystem::permissions(this->state() / "journal", std::filesystem::perms::owner_read | std::filesystem::perms::group_read,
                                     std::filesystem::perm_options::replace);

        if (std::filesystem::status(this->state() / "journal").permissions() !=
            (std::filesystem::perms::owner_read | std::filesystem::perms::group_read))
        {
            GTEST_SKIP() << "the host does not enforce read-only files";
        }

        EXPECT_THROW(this->make_provider(), std::runtime_error);

        std::filesystem::permissions(this->state() / "journal", std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    }

    TEST_F(LayeredProviderTest, AJournalThatCannotBeReadIsReportedInsteadOfIgnored)
    {
        write_host_file(this->state() / "journal", "H\tfile.txt\n");
        std::filesystem::permissions(this->state() / "journal", std::filesystem::perms::none, std::filesystem::perm_options::replace);

        if (std::ifstream{this->state() / "journal"})
        {
            std::filesystem::permissions(this->state() / "journal", std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace);
            GTEST_SKIP() << "the host does not enforce unreadable files";
        }

        EXPECT_THROW(this->make_provider(), std::runtime_error);

        std::filesystem::permissions(this->state() / "journal", std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    }

    TEST_F(LayeredProviderTest, RenamingAnUpperFileHidesItsLowerCounterpart)
    {
        const auto provider = this->make_provider();

        {
            const auto result = provider->open(guest({u"file.txt"}), u"r+b");
            ASSERT_TRUE(result.file);
            ASSERT_EQ(1u, write_text(*result.file, "L"));
        }

        ASSERT_EQ(STATUS_SUCCESS, provider->rename(guest({u"file.txt"}), guest({u"renamed.txt"})));
        EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
        EXPECT_EQ("Lower contents", read_host_file(this->upper() / "renamed.txt"));
    }

    TEST_F(LayeredProviderTest, DirectoriesWithLowerContentCannotBeRenamedYet)
    {
        const auto provider = this->make_provider();

        EXPECT_EQ(STATUS_NOT_SAME_DEVICE, provider->rename(guest({u"dir"}), guest({u"moved"})));
        EXPECT_TRUE(provider->stat(guest({u"dir"})));

        ASSERT_EQ(STATUS_SUCCESS, provider->create_directory(guest({u"fresh"})));
        EXPECT_EQ(STATUS_SUCCESS, provider->rename(guest({u"fresh"}), guest({u"fresher"})));
        EXPECT_TRUE(provider->stat(guest({u"fresher"})));
        EXPECT_FALSE(provider->stat(guest({u"fresh"})));
    }

    TEST_F(LayeredProviderTest, ARecordCutShortByACrashIsIgnored)
    {
        write_host_file(this->state() / "journal", "H\tfile.txt\nH\tdir\\ol");

        {
            const auto provider = this->make_provider();
            EXPECT_FALSE(provider->stat(guest({u"file.txt"})));
            EXPECT_TRUE(provider->stat(guest({u"dir", u"old.txt"})));
            ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"old.txt"})));
        }

        const auto restarted = this->make_provider();
        EXPECT_FALSE(restarted->stat(guest({u"file.txt"})));
        EXPECT_FALSE(restarted->stat(guest({u"dir", u"old.txt"})));
        EXPECT_TRUE(restarted->stat(guest({u"dir", u"keep.txt"})));
    }

    TEST_F(LayeredProviderTest, CopiesAbandonedByACrashAreDiscarded)
    {
        write_host_file(this->state() / "tmp" / "0", "half a copy");

        const auto provider = this->make_provider();
        EXPECT_FALSE(std::filesystem::exists(this->state() / "tmp" / "0"));
        EXPECT_FALSE(provider->stat(guest({u"0"})));
    }

    TEST_F(LayeredProviderTest, TheUpperIsNeverVisibleThroughTheStateDirectory)
    {
        const auto provider = this->make_provider();

        ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"file.txt"})));
        EXPECT_FALSE(provider->stat(guest({u"journal"})));
        EXPECT_FALSE(provider->stat(guest({u"tmp"})));
        EXPECT_EQ(0u, names_in(*provider, {}).count(u"journal"));
    }

    TEST_F(LayeredProviderTest, CopyOnWriteMountsAcceptWritesWithoutTouchingTheLower)
    {
        file_system fs{std::filesystem::path{}, filesystem_mode::read_only};
        fs.mount(windows_path{'c', {}}, this->make_provider(), filesystem_mode::copy_on_write);

        EXPECT_TRUE(fs.is_writable(windows_path{'c', {u"file.txt"}}));

        const auto result = fs.open(windows_path{'c', {u"file.txt"}}, u"r+b");
        ASSERT_TRUE(result.file);
        EXPECT_EQ(1u, write_text(*result.file, "L"));
        ASSERT_TRUE(result.file->flush());

        EXPECT_EQ("Lower contents", read_host_file(this->upper() / "file.txt"));
        EXPECT_EQ("lower contents", read_host_file(this->lower() / "file.txt"));
        EXPECT_EQ(STATUS_SUCCESS, fs.remove(windows_path{'c', {u"dir", u"old.txt"}}));
        EXPECT_THROW(fs.map(windows_path{'d', {}}, this->root, filesystem_mode::copy_on_write), std::runtime_error);
    }

    TEST_F(LayeredProviderTest, RemovalsInOneProcessHideTheFileInTheOther)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        ASSERT_TRUE(second->stat(guest({u"file.txt"})));
        ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"file.txt"})));

        EXPECT_FALSE(second->stat(guest({u"file.txt"})));
        EXPECT_EQ(0u, names_in(*second, {}).count(u"file.txt"));
        EXPECT_EQ(STATUS_OBJECT_NAME_NOT_FOUND, second->open(guest({u"file.txt"}), u"rb").status);
    }

    TEST_F(LayeredProviderTest, ACopyUpInOneProcessIsReadByTheOther)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        {
            const auto result = first->open(guest({u"file.txt"}), u"r+b");
            ASSERT_TRUE(result.file);
            ASSERT_EQ(1u, write_text(*result.file, "L"));
        }

        const auto result = second->open(guest({u"file.txt"}), u"rb");
        ASSERT_TRUE(result.file);
        EXPECT_EQ("Lower contents", read_all(*result.file));
    }

    TEST_F(LayeredProviderTest, WritersInTwoProcessesShareOneCopyOfALowerFile)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        const auto mine = first->open(guest({u"file.txt"}), u"r+b");
        const auto theirs = second->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(mine.file);
        ASSERT_TRUE(theirs.file);

        ASSERT_TRUE(theirs.file->seek(6, SEEK_SET));
        EXPECT_EQ(3u, write_text(*mine.file, "AAA"));
        EXPECT_EQ(3u, write_text(*theirs.file, "BBB"));
        ASSERT_TRUE(mine.file->flush());
        ASSERT_TRUE(theirs.file->flush());

        EXPECT_EQ("AAAer BBBtents", read_host_file(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, ARenameInOneProcessInvalidatesHandlesOfTheOther)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        const auto stale = second->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(stale.file);
        ASSERT_EQ(STATUS_SUCCESS, first->rename(guest({u"file.txt"}), guest({u"moved.txt"})));

        EXPECT_EQ(0u, write_text(*stale.file, "X"));
        EXPECT_FALSE(second->stat(guest({u"file.txt"})));
        EXPECT_TRUE(second->stat(guest({u"moved.txt"})));
        EXPECT_FALSE(std::filesystem::exists(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, AFileRecreatedInOneProcessIsNotModifiedThroughTheOther)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        const auto stale = second->open(guest({u"file.txt"}), u"r+b");
        ASSERT_TRUE(stale.file);

        ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"file.txt"})));
        {
            const auto recreated = first->open(guest({u"file.txt"}), u"wb");
            ASSERT_TRUE(recreated.file);
            ASSERT_EQ(3u, write_text(*recreated.file, "new"));
        }

        EXPECT_EQ(0u, write_text(*stale.file, "X"));
        EXPECT_EQ("new", read_host_file(this->upper() / "file.txt"));
    }

    TEST_F(LayeredProviderTest, ADirectoryRecreatedInOneProcessStaysEmptyInTheOther)
    {
        const auto first = this->make_provider();
        const auto second = this->make_provider();

        ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"dir", u"old.txt"})));
        ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"dir", u"keep.txt"})));
        ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"dir"})));
        ASSERT_EQ(STATUS_SUCCESS, first->create_directory(guest({u"dir"})));

        EXPECT_TRUE(second->stat(guest({u"dir"})));
        EXPECT_TRUE(names_in(*second, guest({u"dir"})).empty());
        EXPECT_EQ(STATUS_OBJECT_NAME_COLLISION, second->create_directory(guest({u"dir"})));
    }

    TEST_F(LayeredProviderTest, RecordsFromBothProcessesSurviveARestart)
    {
        {
            const auto first = this->make_provider();
            const auto second = this->make_provider();

            ASSERT_EQ(STATUS_SUCCESS, first->remove(guest({u"file.txt"})));
            ASSERT_EQ(STATUS_SUCCESS, second->remove(guest({u"dir", u"old.txt"})));
            ASSERT_EQ(STATUS_SUCCESS, first->rename(guest({u"dir", u"keep.txt"}), guest({u"kept.txt"})));
        }

        const auto restarted = this->make_provider();
        EXPECT_FALSE(restarted->stat(guest({u"file.txt"})));
        EXPECT_FALSE(restarted->stat(guest({u"dir", u"old.txt"})));
        EXPECT_FALSE(restarted->stat(guest({u"dir", u"keep.txt"})));
        EXPECT_EQ("keep", read_host_file(this->upper() / "kept.txt"));
    }

    TEST_F(LayeredProviderTest, AnUnfinishedRecordOfAnotherWriterIsCutBeforeAppending)
    {
        const auto provider = this->make_provider();

        {
            std::ofstream journal(this->state() / "journal", std::ios::binary | std::ios::app);
            journal << "H\tdir\\ke";
        }

        EXPECT_TRUE(provider->stat(guest({u"dir", u"keep.txt"})));
        ASSERT_EQ(STATUS_SUCCESS, provider->remove(guest({u"dir", u"old.txt"})));

        const auto restarted = this->make_provider();
        EXPECT_FALSE(restarted->stat(guest({u"dir", u"old.txt"})));
        EXPECT_TRUE(restarted->stat(guest({u"dir", u"keep.txt"})));
    }
}
