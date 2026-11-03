#include <gtest/gtest.h>

#include <emulator_utils.hpp>
#include <snapshot.hpp>
#include <sogen_version.hpp>

#include <cstring>
#include <fstream>
#include <random>

namespace sogen::test
{
    namespace
    {
        constexpr size_t header_size = 88;
        constexpr size_t build_id_offset = 4;
        constexpr size_t build_id_size = 64;

        class SnapshotIdentityTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->file = std::filesystem::temp_directory_path() /
                             ("sogen-snapshot-identity-" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove(this->file, ec);
            }

            void write(const std::vector<char>& bytes) const
            {
                std::ofstream stream(this->file, std::ios::binary);
                stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }

            std::string load_error() const
            {
                try
                {
                    (void)snapshot::get_snapshot_machine(this->file);
                }
                catch (const std::exception& e)
                {
                    return e.what();
                }

                return {};
            }

            std::filesystem::path file{};
        };

        std::vector<char> header_with_build_id(const std::string_view build_id)
        {
            std::vector<char> header(header_size);
            std::memcpy(header.data(), "SNAP", 4);
            std::memcpy(header.data() + build_id_offset, build_id.data(), std::min(build_id.size(), build_id_size));
            return header;
        }
    }

    TEST_F(SnapshotIdentityTest, ASnapshotOfAnotherBuildIsRejectedFromItsHeader)
    {
        this->write(header_with_build_id("0123456789abcdef0123456789abcdef01234567"));

        const auto error = this->load_error();
        if (version::build_id.empty())
        {
            EXPECT_NE(error.find("build identity"), std::string::npos) << error;
            return;
        }

        EXPECT_NE(error.find("another Sogen build"), std::string::npos) << error;
        EXPECT_NE(error.find("0123456789ab"), std::string::npos) << error;
    }

    TEST_F(SnapshotIdentityTest, ASnapshotWithTheOldVersionNumberIsReportedAsOld)
    {
        std::vector<char> legacy(header_size);
        std::memcpy(legacy.data(), "SNAP", 4);
        legacy[build_id_offset] = 20;
        this->write(legacy);

        const auto error = this->load_error();
        EXPECT_NE(error.find("older Sogen build"), std::string::npos) << error;
    }

    TEST_F(SnapshotIdentityTest, AMalformedBuildIdIsNotTakenForOne)
    {
        this->write(header_with_build_id("abc\ndef"));
        EXPECT_NE(this->load_error().find("older Sogen build"), std::string::npos);

        auto unterminated = header_with_build_id(std::string(build_id_size, 'a'));
        this->write(unterminated);
        EXPECT_NE(this->load_error().find("older Sogen build"), std::string::npos);
    }
}
