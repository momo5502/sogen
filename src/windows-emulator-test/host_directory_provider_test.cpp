#include <gtest/gtest.h>

#include <filesystem/host_directory_provider.hpp>

#include <fstream>
#include <random>
#include <set>

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

        windows_path guest(std::initializer_list<std::u16string> folders)
        {
            return windows_path{std::nullopt, std::list<std::u16string>(folders)};
        }

        class HostDirectoryLinksTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->root_ = std::filesystem::temp_directory_path() /
                              ("sogen-host-links-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
                write_host_file(this->target() / "inside.txt", "inside");
                write_host_file(this->outside() / "secret.txt", "secret");
                this->root_ = std::filesystem::canonical(this->root_);

                std::error_code ec{};
                std::filesystem::create_directory_symlink(this->outside(), this->target() / "escape", ec);
                if (!ec)
                {
                    std::filesystem::create_symlink(this->outside() / "secret.txt", this->target() / "secret.txt", ec);
                }
                if (ec)
                {
                    GTEST_SKIP() << "Cannot create symlinks here: " << ec.message();
                }
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root_, ec);
            }

            std::filesystem::path target() const
            {
                return this->root_ / "target";
            }

            std::filesystem::path outside() const
            {
                return this->root_ / "outside";
            }

            static std::set<std::u16string> names_in(const host_directory_provider& provider)
            {
                std::set<std::u16string> names{};
                for (const auto& entry : provider.list(guest({})))
                {
                    names.insert(entry.name);
                }

                const auto listed_names = provider.list_names(guest({}));
                EXPECT_EQ(names, (std::set<std::u16string>{listed_names.begin(), listed_names.end()}));

                return names;
            }

          private:
            std::filesystem::path root_{};
        };

        TEST_F(HostDirectoryLinksTest, RefusedLinksDoNotReachOutsideTheTarget)
        {
            host_directory_provider provider{this->target(), host_links::refuse};

            EXPECT_FALSE(provider.stat(guest({u"escape", u"secret.txt"})));
            EXPECT_FALSE(provider.stat(guest({u"secret.txt"})));
            EXPECT_NE(provider.open(guest({u"escape", u"secret.txt"}), u"rb").status, STATUS_SUCCESS);
            EXPECT_NE(provider.open(guest({u"secret.txt"}), u"rb").status, STATUS_SUCCESS);
            EXPECT_EQ(names_in(provider), std::set<std::u16string>{u"inside.txt"});
            EXPECT_EQ(provider.open(guest({u"inside.txt"}), u"rb").status, STATUS_SUCCESS);
            EXPECT_EQ(provider.open(guest({u"INSIDE.TXT"}), u"rb").status, STATUS_SUCCESS);
            EXPECT_FALSE(provider.stat(guest({u"Escape", u"Secret.txt"})));
            EXPECT_NE(provider.open(guest({u"SECRET.TXT"}), u"rb").status, STATUS_SUCCESS);
        }

        TEST_F(HostDirectoryLinksTest, FollowedLinksReachTheirTargets)
        {
            host_directory_provider provider{this->target()};

            EXPECT_EQ(provider.open(guest({u"escape", u"secret.txt"}), u"rb").status, STATUS_SUCCESS);
            EXPECT_EQ(names_in(provider), (std::set<std::u16string>{u"escape", u"inside.txt", u"secret.txt"}));
        }

        TEST(HostDirectoryDeleteSharingTest, OpenStreamsDoNotBlockRenameOrRemove)
        {
            const auto root =
                std::filesystem::temp_directory_path() /
                ("sogen-host-delete-sharing-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
            std::filesystem::create_directories(root);
            host_directory_provider provider{root};

            for (const auto* mode : {u"rb", u"r+b", u"w+b", u"ab"})
            {
                write_host_file(root / "source.bin", "data");
                const auto opened = provider.open(guest({u"source.bin"}), mode);
                ASSERT_EQ(STATUS_SUCCESS, opened.status);

                EXPECT_EQ(STATUS_SUCCESS, provider.rename(guest({u"source.bin"}), guest({u"renamed.bin"})));
                EXPECT_FALSE(std::filesystem::exists(root / "source.bin"));
                EXPECT_EQ(STATUS_SUCCESS, provider.remove(guest({u"renamed.bin"})));
                EXPECT_FALSE(std::filesystem::exists(root / "renamed.bin"));
            }

            std::error_code ec{};
            std::filesystem::remove_all(root, ec);
        }

        TEST(HostDirectoryProviderTest, KeepsThousandsOfFilesOpen)
        {
            const auto root =
                std::filesystem::temp_directory_path() /
                ("sogen-host-provider-test-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
            write_host_file(root / "package.pkg", "data");

            {
                host_directory_provider provider{root};
                std::vector<std::unique_ptr<i_file>> handles{};
                for (size_t i = 0; i < 2000; ++i)
                {
                    auto result = provider.open(guest({u"package.pkg"}), u"rb");
                    ASSERT_EQ(result.status, STATUS_SUCCESS) << "open " << i;
                    handles.push_back(std::move(result.file));
                }
            }

            std::error_code ec{};
            std::filesystem::remove_all(root, ec);
        }
    } // namespace
} // namespace sogen::test
