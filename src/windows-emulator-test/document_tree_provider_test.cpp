#include <gtest/gtest.h>

#include <emulator_utils.hpp>
#include <filesystem/document_tree_provider.hpp>
#include <filesystem/layered_provider.hpp>

#include <cstring>
#include <map>
#include <random>
#include <sys/stat.h>

namespace sogen::test
{
    namespace
    {
        class memory_file final : public i_file
        {
          public:
            explicit memory_file(std::string data)
                : data_(std::move(data))
            {
            }

            size_t read(const std::span<std::byte> buffer) override
            {
                const auto count = std::min(buffer.size(), this->data_.size() - this->position_);
                std::memcpy(buffer.data(), this->data_.data() + this->position_, count);
                this->position_ += count;
                return count;
            }

            size_t write(std::span<const std::byte> /*buffer*/) override
            {
                return 0;
            }

            bool flush() override
            {
                return true;
            }

            bool seek(const int64_t offset, const int origin) override
            {
                const auto base = origin == SEEK_SET ? 0 : (origin == SEEK_CUR ? static_cast<int64_t>(this->position_) : this->size());
                const auto target = base + offset;
                if (target < 0 || target > this->size())
                {
                    return false;
                }

                this->position_ = static_cast<size_t>(target);
                return true;
            }

            int64_t tell() override
            {
                return static_cast<int64_t>(this->position_);
            }

            int64_t size() override
            {
                return static_cast<int64_t>(this->data_.size());
            }

            bool resize(uint64_t /*size*/) override
            {
                return false;
            }

            std::optional<struct compat_stat> stat() override
            {
                struct compat_stat result{};
                result.st_mode = S_IFREG;
                result.st_size = this->size();
                return result;
            }

          private:
            std::string data_{};
            size_t position_{};
        };

        class memory_source final : public document_source
        {
          public:
            std::unique_ptr<i_file> open(const std::string& document_id) override
            {
                this->opened.push_back(document_id);
                const auto file = this->files.find(document_id);
                if (file == this->files.end())
                {
                    return nullptr;
                }

                return std::make_unique<memory_file>(file->second);
            }

            std::map<std::string, std::string> files{};
            std::vector<std::string> opened{};
        };

        windows_path guest(std::initializer_list<std::u16string> folders)
        {
            return windows_path{std::nullopt, std::list<std::u16string>(folders)};
        }

        document_entry file_entry(std::u16string path, std::string id, const uint64_t size)
        {
            return {.path = std::move(path), .id = std::move(id), .size = size, .modified_ms = 1700000000123};
        }

        document_entry directory_entry_for(std::u16string path, std::string id)
        {
            return {.path = std::move(path), .id = std::move(id), .is_directory = true};
        }

        std::string read_all(i_file& file)
        {
            std::string contents(static_cast<size_t>(file.size()), '\0');
            contents.resize(file.read(std::as_writable_bytes(std::span(contents))));
            return contents;
        }

        std::shared_ptr<memory_source> make_source()
        {
            auto source = std::make_shared<memory_source>();
            source->files["primary:Game/Game.exe"] = "MZ executable";
            source->files["primary:Game/Data/Main.pak"] = "pack";
            return source;
        }

        std::vector<document_entry> game_entries()
        {
            return {
                file_entry(u"Game.exe", "primary:Game/Game.exe", 13),
                directory_entry_for(u"Data", "primary:Game/Data"),
                file_entry(u"Data/Main.pak", "primary:Game/Data/Main.pak", 4),
            };
        }
    }

    TEST(DocumentTreeProviderTest, LooksUpPathsIgnoringCase)
    {
        const auto source = make_source();
        document_tree_provider provider{game_entries(), source};

        const auto stat = provider.stat(guest({u"DATA", u"main.PAK"}));
        ASSERT_TRUE(stat.has_value());
        EXPECT_EQ(stat->st_mode & S_IFMT, S_IFREG);
        EXPECT_EQ(stat->st_size, 4);
        EXPECT_EQ(stat->st_mtimespec.tv_sec, 1700000000);
        EXPECT_EQ(stat->st_mtimespec.tv_nsec, 123000000);

        auto opened = provider.open(guest({u"game.exe"}), u"rb");
        ASSERT_EQ(opened.status, STATUS_SUCCESS);
        EXPECT_EQ(read_all(*opened.file), "MZ executable");
        EXPECT_EQ(source->opened, std::vector<std::string>{"primary:Game/Game.exe"});
    }

    TEST(DocumentTreeProviderTest, ListsChildrenWithTheirOriginalCase)
    {
        document_tree_provider provider{game_entries(), make_source()};

        std::map<std::u16string, bool> root{};
        for (const auto& entry : provider.list(guest({})))
        {
            root.emplace(entry.name, entry.is_directory);
        }

        EXPECT_EQ(root, (std::map<std::u16string, bool>{{u"Data", true}, {u"Game.exe", false}}));

        const auto data = provider.list(guest({u"data"}));
        ASSERT_EQ(data.size(), 1u);
        EXPECT_EQ(data.front().name, u"Main.pak");
    }

    TEST(DocumentTreeProviderTest, FileIdsFollowTheDocumentId)
    {
        document_tree_provider provider{game_entries(), make_source()};

        const auto game = provider.stat(guest({u"game.exe"}));
        const auto pack = provider.stat(guest({u"data", u"main.pak"}));
        ASSERT_TRUE(game.has_value() && pack.has_value());
        EXPECT_NE(game->st_ino, pack->st_ino);

        auto opened = provider.open(guest({u"game.exe"}), u"rb");
        ASSERT_EQ(opened.status, STATUS_SUCCESS);
        const auto handle = opened.file->stat();
        ASSERT_TRUE(handle.has_value());
        EXPECT_EQ(handle->st_ino, game->st_ino);
        EXPECT_EQ(handle->st_size, 13);

        auto renamed = game_entries();
        renamed[0].path = u"Renamed.exe";
        document_tree_provider moved{std::move(renamed), make_source()};
        EXPECT_EQ(moved.stat(guest({u"renamed.exe"}))->st_ino, game->st_ino);
    }

    TEST(DocumentTreeProviderTest, IsReadOnly)
    {
        document_tree_provider provider{game_entries(), make_source()};

        EXPECT_EQ(provider.open(guest({u"game.exe"}), u"r+b").status, STATUS_ACCESS_DENIED);
        EXPECT_EQ(provider.open(guest({u"new.txt"}), u"wb").status, STATUS_ACCESS_DENIED);
        EXPECT_EQ(provider.create_directory(guest({u"saves"})), STATUS_ACCESS_DENIED);
        EXPECT_EQ(provider.remove(guest({u"game.exe"})), STATUS_ACCESS_DENIED);
        EXPECT_EQ(provider.rename(guest({u"game.exe"}), guest({u"other.exe"})), STATUS_ACCESS_DENIED);
    }

    TEST(DocumentTreeProviderTest, OpeningADirectoryOrMissingFileFails)
    {
        document_tree_provider provider{game_entries(), make_source()};

        EXPECT_EQ(provider.open(guest({u"data"}), u"rb").status, STATUS_FILE_IS_A_DIRECTORY);
        EXPECT_EQ(provider.open(guest({u"missing.txt"}), u"rb").status, STATUS_OBJECT_NAME_NOT_FOUND);
    }

    TEST(DocumentTreeProviderTest, RejectsPathsThatDifferOnlyInCase)
    {
        auto entries = game_entries();
        entries.push_back(file_entry(u"GAME.exe", "primary:Game/GAME.exe", 1));

        EXPECT_THROW((document_tree_provider{std::move(entries), make_source()}), std::runtime_error);
    }

    TEST(DocumentTreeProviderTest, RejectsMalformedListings)
    {
        EXPECT_THROW((document_tree_provider{{file_entry(u"Missing/File.txt", "a", 1)}, make_source()}), std::runtime_error);
        EXPECT_THROW((document_tree_provider{{file_entry(u"../escape.txt", "a", 1)}, make_source()}), std::runtime_error);
        EXPECT_THROW((document_tree_provider{{file_entry(u"a\\b.txt", "a", 1)}, make_source()}), std::runtime_error);
        EXPECT_THROW((document_tree_provider{{file_entry(u"Game.exe", "a", 1), directory_entry_for(u"Game.exe/x", "b")}, make_source()}),
                     std::runtime_error);
    }

    TEST(DocumentTreeProviderTest, WritesThroughALayerLandInTheUpper)
    {
        const auto root = std::filesystem::temp_directory_path() /
                          ("sogen-document-tree-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(root);

        {
            const auto source = make_source();
            layered_provider provider{std::make_unique<document_tree_provider>(game_entries(), source), root / "upper", root / "state"};

            auto opened = provider.open(guest({u"data", u"main.pak"}), u"r+b");
            ASSERT_EQ(opened.status, STATUS_SUCCESS);
            const std::string_view text = "PACK";
            EXPECT_EQ(opened.file->write(std::as_bytes(std::span(text))), text.size());
            opened.file.reset();

            auto reread = provider.open(guest({u"data", u"main.pak"}), u"rb");
            ASSERT_EQ(reread.status, STATUS_SUCCESS);
            EXPECT_EQ(read_all(*reread.file), "PACK");
            EXPECT_EQ(source->files["primary:Game/Data/Main.pak"], "pack");

            EXPECT_EQ(provider.remove(guest({u"game.exe"})), STATUS_SUCCESS);
            EXPECT_FALSE(provider.stat(guest({u"game.exe"})).has_value());
        }

        std::error_code ec{};
        std::filesystem::remove_all(root, ec);
    }

} // namespace sogen::test
