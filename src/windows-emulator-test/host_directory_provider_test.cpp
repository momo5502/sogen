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
    } // namespace
} // namespace sogen::test
