#if defined(__linux__)

#include <gtest/gtest.h>

#include <emulator_utils.hpp>
#include <filesystem/document_broker.hpp>

#include <fcntl.h>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <unistd.h>

namespace sogen::test
{
    namespace
    {
        std::string read_all(i_file& file)
        {
            std::string contents(static_cast<size_t>(file.size()), '\0');
            contents.resize(file.read(std::as_writable_bytes(std::span(contents))));
            return contents;
        }

        class DocumentBrokerTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root =
                    std::filesystem::temp_directory_path() /
                    ("sogen-document-broker-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                std::filesystem::create_directories(this->root);

                std::ofstream{this->root / "game.exe", std::ios::binary} << "MZ executable";
                std::ofstream{this->root / "data.pak", std::ios::binary} << "0123456789";
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            std::shared_ptr<descriptor_document_source> make_source(const size_t capacity = 8)
            {
                return std::make_shared<descriptor_document_source>(
                    [this](const std::string& document_id) {
                        ++this->opens;
                        if (document_id == "pipe")
                        {
                            std::array<int, 2> pipe_fds{};
                            if (::pipe(pipe_fds.data()) != 0)
                            {
                                return unique_fd{};
                            }

                            ::close(pipe_fds[1]);
                            return unique_fd{pipe_fds[0]};
                        }

                        return unique_fd{::open((this->root / document_id).c_str(), O_RDONLY | O_CLOEXEC)};
                    },
                    capacity);
            }

            std::vector<document_entry> entries() const
            {
                return {
                    {.path = u"Game.exe", .id = "game.exe", .size = 13},
                    {.path = u"Data.pak", .id = "data.pak", .size = 10},
                    {.path = u"Pipe", .id = "pipe"},
                    {.path = u"Folder", .id = "folder", .is_directory = true},
                };
            }

            std::filesystem::path root{};
            int opens{};
        };
    }

    TEST_F(DocumentBrokerTest, ReadsByOffsetWithIndependentPositions)
    {
        const auto source = this->make_source();
        auto first = source->open("data.pak");
        auto second = source->open("data.pak");
        ASSERT_TRUE(first && second);
        EXPECT_EQ(this->opens, 1);

        ASSERT_TRUE(first->seek(6, SEEK_SET));
        std::array<std::byte, 2> buffer{};
        EXPECT_EQ(second->read(buffer), 2u);
        EXPECT_EQ(first->read(buffer), 2u);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), buffer.size()), "67");
        EXPECT_EQ(second->tell(), 2);

        ASSERT_TRUE(first->seek(-1, SEEK_END));
        EXPECT_EQ(read_all(*first).substr(0, 1), "9");
        EXPECT_FALSE(first->seek(-1, SEEK_SET));
        EXPECT_FALSE(first->seek(std::numeric_limits<int64_t>::max(), SEEK_END));
        EXPECT_TRUE(first->seek(std::numeric_limits<int64_t>::max(), SEEK_SET));
        EXPECT_EQ(first->read(buffer), 0u);
        EXPECT_EQ(first->write(std::as_bytes(std::span("x", 1))), 0u);
    }

    TEST_F(DocumentBrokerTest, EvictedDescriptorsStayOpenWhileInUse)
    {
        const auto source = this->make_source(1);
        auto game = source->open("game.exe");
        auto data = source->open("data.pak");
        ASSERT_TRUE(game && data);

        EXPECT_EQ(read_all(*game), "MZ executable");
        EXPECT_EQ(this->opens, 2);

        auto again = source->open("game.exe");
        EXPECT_EQ(this->opens, 3);
    }

    TEST_F(DocumentBrokerTest, RejectsFilesThatCannotBeReadByOffset)
    {
        EXPECT_EQ(this->make_source()->open("pipe"), nullptr);
    }

    TEST_F(DocumentBrokerTest, ServesTheListingAndDescriptorsToClients)
    {
        document_broker broker{this->entries(), this->make_source()};
        auto tree = connect_document_broker(broker.name(), broker.token());

        ASSERT_EQ(tree.entries.size(), 4u);
        EXPECT_EQ(tree.entries[0].path, u"Game.exe");
        EXPECT_TRUE(tree.entries[3].is_directory);

        auto file = tree.source->open("game.exe");
        ASSERT_NE(file, nullptr);
        EXPECT_EQ(read_all(*file), "MZ executable");

        std::vector<std::thread> readers{};
        std::atomic_int matches{};
        for (int i = 0; i < 4; ++i)
        {
            readers.emplace_back([&] {
                for (int j = 0; j < 25; ++j)
                {
                    auto data = tree.source->open("data.pak");
                    if (data && data->seek(j % 10, SEEK_SET))
                    {
                        std::array<std::byte, 1> byte{};
                        if (data->read(byte) == 1 && static_cast<char>(byte[0]) == static_cast<char>('0' + (j % 10)))
                        {
                            ++matches;
                        }
                    }
                }
            });
        }

        for (auto& reader : readers)
        {
            reader.join();
        }

        EXPECT_EQ(matches, 100);
    }

    TEST_F(DocumentBrokerTest, RefusesDocumentsOutsideTheListing)
    {
        document_broker broker{this->entries(), this->make_source()};
        auto tree = connect_document_broker(broker.name(), broker.token());

        EXPECT_EQ(tree.source->open("../etc/passwd"), nullptr);
        EXPECT_EQ(tree.source->open("folder"), nullptr);
        EXPECT_EQ(tree.source->open("pipe"), nullptr);
        EXPECT_NE(tree.source->open("data.pak"), nullptr);
    }

    TEST_F(DocumentBrokerTest, RefusesClientsWithoutTheToken)
    {
        document_broker broker{this->entries(), this->make_source()};
        EXPECT_THROW(connect_document_broker(broker.name(), "wrong"), std::runtime_error);
    }

} // namespace sogen::test

#endif
