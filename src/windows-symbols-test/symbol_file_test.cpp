#include <gtest/gtest.h>

#include <windows-symbols/pdb_file.hpp>
#include <windows-symbols/cab_file.hpp>
#include <utils/io.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        TEST(PdbSignature, AcceptsOnlyPlainPortableFilenames)
        {
            EXPECT_TRUE((pdb_signature{.path = R"(C:\build\symbols\ntdll.pdb)"}).has_safe_filename());
            EXPECT_TRUE((pdb_signature{.path = "ntdll.pdb"}).has_safe_filename());
            EXPECT_FALSE((pdb_signature{.path = R"(C:\build\..)"}).has_safe_filename());
            EXPECT_FALSE((pdb_signature{.path = R"(C:\build\.)"}).has_safe_filename());
            EXPECT_FALSE((pdb_signature{.path = R"(C:\build\)"}).has_safe_filename());
            EXPECT_FALSE((pdb_signature{.path = "D:ntdll.pdb"}).has_safe_filename());
            EXPECT_TRUE((pdb_signature{.path = "D:\\ntdll.pdb"}).has_safe_filename());
        }

        struct file_bytes
        {
            std::vector<std::byte> data{};

            explicit file_bytes(const size_t size)
                : data(size)
            {
            }

            void integer(const size_t offset, const uint32_t value, const size_t width = 4)
            {
                for (size_t i = 0; i < width; ++i)
                {
                    this->data.at(offset + i) = static_cast<std::byte>((value >> (i * 8)) & 0xff);
                }
            }

            void text(const size_t offset, const std::string_view value)
            {
                for (size_t i = 0; i < value.size(); ++i)
                {
                    this->data.at(offset + i) = static_cast<std::byte>(value[i]);
                }
            }
        };

        file_bytes make_pdb()
        {
            file_bytes pdb{9 * 512};
            pdb.text(0, "Microsoft C/C++ MSF 7.00\r\n\x1a"
                        "DS");
            pdb.integer(32, 512);
            pdb.integer(36, 1);
            pdb.integer(40, 9);
            pdb.integer(44, 44);
            pdb.integer(52, 3);
            pdb.integer(3 * 512, 4);
            const std::vector<uint32_t> directory{6, 0, 28, 0, 132, 32, 60, 5, 6, 7, 8};
            for (size_t i = 0; i < directory.size(); ++i)
            {
                pdb.integer(4 * 512 + i * 4, directory[i]);
            }
            const size_t info = 5 * 512;
            pdb.integer(info, 20000404);
            pdb.integer(info + 8, 99);
            pdb.integer(info + 12, 0x12345678);
            pdb.integer(info + 16, 0x9abc, 2);
            pdb.integer(info + 18, 0xdef0, 2);
            pdb.text(info + 20, "12345678");
            const size_t dbi = 6 * 512;
            pdb.integer(dbi, 0xffffffff);
            pdb.integer(dbi + 4, 19990903);
            pdb.integer(dbi + 8, 7);
            pdb.integer(dbi + 12, 0xffff, 2);
            pdb.integer(dbi + 16, 0xffff, 2);
            pdb.integer(dbi + 20, 4, 2);
            pdb.integer(dbi + 24, 68);
            pdb.integer(dbi + 64 + 34, 5, 2);
            pdb.integer(dbi + 64 + 36, 60);
            const size_t public_record = 7 * 512;
            pdb.integer(public_record, 30, 2);
            pdb.integer(public_record + 2, 0x110e, 2);
            pdb.integer(public_record + 4, 2);
            pdb.integer(public_record + 8, 0x20);
            pdb.integer(public_record + 12, 1, 2);
            pdb.text(public_record + 14, "PublicFunction");
            const size_t module = 8 * 512;
            pdb.integer(module, 4);
            pdb.integer(module + 4, 53, 2);
            pdb.integer(module + 6, 0x110f, 2);
            pdb.integer(module + 8 + 28, 0x40);
            pdb.integer(module + 8 + 32, 1, 2);
            pdb.text(module + 8 + 35, "PrivateFunction");
            return pdb;
        }

        file_bytes make_mszip_cab(const std::string_view filename, const std::string_view contents)
        {
            const size_t data_offset = 44 + 16 + filename.size() + 1;
            const size_t compressed_size = contents.size() + 7;
            file_bytes cab{data_offset + 8 + compressed_size};
            cab.text(0, "MSCF");
            cab.integer(8, static_cast<uint32_t>(cab.data.size()));
            cab.integer(16, 44);
            cab.integer(24, 0x0103, 2);
            cab.integer(26, 1, 2);
            cab.integer(28, 1, 2);
            cab.integer(36, static_cast<uint32_t>(data_offset));
            cab.integer(40, 1, 2);
            cab.integer(42, 1, 2);
            cab.integer(44, static_cast<uint32_t>(contents.size()));
            cab.text(60, filename);
            cab.integer(data_offset + 4, static_cast<uint32_t>(compressed_size), 2);
            cab.integer(data_offset + 6, static_cast<uint32_t>(contents.size()), 2);
            cab.text(data_offset + 8, "CK");
            cab.integer(data_offset + 10, 1, 1);
            cab.integer(data_offset + 11, static_cast<uint32_t>(contents.size()), 2);
            cab.integer(data_offset + 13, static_cast<uint32_t>(contents.size()) ^ 0xffff, 2);
            cab.text(data_offset + 15, contents);
            return cab;
        }

        class symbol_file_test : public testing::Test
        {
          protected:
            std::filesystem::path root{};

            void SetUp() override
            {
                this->root = std::filesystem::temp_directory_path() / ("sogen-symbols-" + std::to_string(std::random_device{}()));
                this->root /= std::filesystem::path{u"s\u00edmbolos"};
                std::filesystem::create_directories(this->root);
            }

            void TearDown() override
            {
                std::error_code ec{};
                std::filesystem::remove_all(this->root, ec);
            }

            std::filesystem::path save(const std::string_view filename, const file_bytes& bytes) const
            {
                const auto path = this->root / filename;
                if (!utils::io::write_file(path, bytes.data))
                {
                    throw std::runtime_error("Failed to write fixture");
                }
                return path;
            }
        };

        TEST_F(symbol_file_test, ReadsPublicAndPrivateCodeViewFunctions)
        {
            symbols::pdb_file pdb{this->save("fixture.pdb", make_pdb())};
            const auto result = pdb.read();
            ASSERT_TRUE(result.success) << result.error;
            EXPECT_EQ(pdb.symbols.at({1, 0x20}), "PublicFunction");
            EXPECT_EQ(pdb.symbols.at({1, 0x40}), "PrivateFunction");
            EXPECT_EQ(pdb.signature.guid, "123456789ABCDEF03132333435363738");
            EXPECT_EQ(pdb.signature.age, 7U);
        }

        TEST_F(symbol_file_test, MatchesGuidAndDbiAge)
        {
            const symbols::pdb_file pdb{this->save("fixture.pdb", make_pdb())};
            pdb_signature expected{.guid = "123456789ABCDEF03132333435363738", .age = 7};
            EXPECT_EQ(pdb.validate(expected).status, symbols::pdb_validation_status::match);
            expected.age = 99;
            EXPECT_EQ(pdb.validate(expected).status, symbols::pdb_validation_status::mismatch);
            expected.age = 7;
            expected.guid[0] = '0';
            EXPECT_EQ(pdb.validate(expected).status, symbols::pdb_validation_status::mismatch);
        }

        TEST_F(symbol_file_test, RejectsTruncatedAndInvalidMsfBlocks)
        {
            auto bytes = make_pdb();
            bytes.integer(3 * 512, 100);
            const pdb_signature expected{.guid = "123456789ABCDEF03132333435363738", .age = 7};
            const symbols::pdb_file invalid{this->save("invalid.pdb", bytes)};
            EXPECT_EQ(invalid.validate(expected).status, symbols::pdb_validation_status::invalid_artifact);
            bytes = make_pdb();
            bytes.data.resize(511);
            const symbols::pdb_file truncated{this->save("truncated.pdb", bytes)};
            EXPECT_EQ(truncated.validate(expected).status, symbols::pdb_validation_status::invalid_artifact);
        }

        TEST_F(symbol_file_test, RejectsCodeViewRecordExtendingPastStream)
        {
            auto bytes = make_pdb();
            bytes.integer(8 * 512 + 4, 0xffff, 2);
            symbols::pdb_file pdb{this->save("invalid-record.pdb", bytes)};
            EXPECT_FALSE(pdb.read().success);
            EXPECT_TRUE(pdb.symbols.empty());
        }

        TEST_F(symbol_file_test, ExtractsRequestedMszipMemberToUnicodePath)
        {
            const auto archive = this->save("fixture.pd_", make_mszip_cab("fixture.pdb", "PDB payload"));
            const auto output = this->root / "expanded.pdb";
            ASSERT_NO_THROW(symbols::extract_cab_file(archive, output, "fixture.pdb"));
            const auto bytes = utils::io::read_file(output);
            EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "PDB payload");
        }

        TEST_F(symbol_file_test, RejectsUnrequestedCabinetPaths)
        {
            const auto archive = this->save("fixture.pd_", make_mszip_cab("../fixture.pdb", "payload"));
            const auto output = this->root / "expanded.pdb";
            EXPECT_THROW(symbols::extract_cab_file(archive, output, "fixture.pdb"), std::runtime_error);
            EXPECT_FALSE(std::filesystem::exists(output));
        }

        TEST_F(symbol_file_test, RemovesPartialOutputForTruncatedCabinet)
        {
            auto bytes = make_mszip_cab("fixture.pdb", "payload");
            bytes.data.pop_back();
            const auto archive = this->save("fixture.pd_", bytes);
            const auto output = this->root / "expanded.pdb";
            EXPECT_THROW(symbols::extract_cab_file(archive, output, "fixture.pdb"), std::runtime_error);
            EXPECT_FALSE(std::filesystem::exists(output));
        }
    }
}
