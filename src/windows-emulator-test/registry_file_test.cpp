#include <gtest/gtest.h>

#include <registry/registry_file.hpp>

#include <atomic>
#include <chrono>
#include <fstream>

namespace sogen::test
{
    namespace
    {
        constexpr size_t hive_data_offset = 0x1000;
        constexpr size_t root_key_offset = hive_data_offset + 0x20;

        void write_uint16(std::vector<std::byte>& data, const size_t offset, const uint16_t value)
        {
            data[offset] = static_cast<std::byte>(value & 0xFF);
            data[offset + 1] = static_cast<std::byte>(value >> 8);
        }

        void write_uint32(std::vector<std::byte>& data, const size_t offset, const uint32_t value)
        {
            data[offset] = static_cast<std::byte>(value & 0xFF);
            data[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<std::byte>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<std::byte>((value >> 24) & 0xFF);
        }

        void write_key(std::vector<std::byte>& data, const size_t offset, const std::string_view name, const uint32_t subkey_list_offset)
        {
            write_uint32(data, offset + 32, subkey_list_offset);
            write_uint16(data, offset + 76, static_cast<uint16_t>(name.size()));
            for (size_t i = 0; i < name.size(); ++i)
            {
                data[offset + 80 + i] = static_cast<std::byte>(name[i]);
            }
        }

        void write_subkey_list(std::vector<std::byte>& data, const uint32_t list_offset, const uint32_t key_offset)
        {
            const auto offset = hive_data_offset + list_offset;
            data[offset + 4] = static_cast<std::byte>('l');
            data[offset + 5] = static_cast<std::byte>('f');
            write_uint16(data, offset + 6, 1);
            write_uint32(data, offset + 8, key_offset);
        }

        std::vector<std::byte> create_software_hive()
        {
            std::vector<std::byte> data(hive_data_offset + 0x1200);
            data[0] = static_cast<std::byte>('r');
            data[1] = static_cast<std::byte>('e');
            data[2] = static_cast<std::byte>('g');
            data[3] = static_cast<std::byte>('f');

            write_key(data, root_key_offset, {}, 0x200);
            write_subkey_list(data, 0x200, 0x400);
            write_key(data, hive_data_offset + 0x400, "Microsoft", 0x600);
            write_subkey_list(data, 0x600, 0x800);
            write_key(data, hive_data_offset + 0x800, "Windows NT", 0xA00);
            write_subkey_list(data, 0xA00, 0xC00);
            write_key(data, hive_data_offset + 0xC00, "CurrentVersion", 0xE00);
            write_subkey_list(data, 0xE00, 0x1000);
            write_key(data, hive_data_offset + 0x1000, "ProfileList", 0);
            return data;
        }

        bool write_hive(const std::filesystem::path& path, const std::span<const std::byte> data)
        {
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return file.good();
        }

        std::vector<std::byte> encode_utf8_with_bom(const std::string_view text)
        {
            std::vector<std::byte> result{std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}};
            result.reserve(result.size() + text.size());
            for (const auto character : text)
            {
                result.push_back(static_cast<std::byte>(static_cast<uint8_t>(character)));
            }
            return result;
        }

        std::vector<std::byte> encode_utf16(const std::u16string_view text, const bool big_endian)
        {
            std::vector<std::byte> result{};
            result.reserve(2 + text.size() * 2);
            result.push_back(big_endian ? std::byte{0xFE} : std::byte{0xFF});
            result.push_back(big_endian ? std::byte{0xFF} : std::byte{0xFE});
            for (const auto character : text)
            {
                const auto low = static_cast<std::byte>(character & 0xFF);
                const auto high = static_cast<std::byte>(character >> 8);
                result.push_back(big_endian ? high : low);
                result.push_back(big_endian ? low : high);
            }
            return result;
        }

        class RegistryFileTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                static std::atomic_uint64_t counter{};
                const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
                directory_ = std::filesystem::temp_directory_path() /
                             ("sogen-registry-file-test-" + std::to_string(timestamp) + "-" + std::to_string(counter++));
                ASSERT_TRUE(std::filesystem::create_directories(directory_));

                const std::array<std::byte, 4> empty_hive{
                    static_cast<std::byte>('r'),
                    static_cast<std::byte>('e'),
                    static_cast<std::byte>('g'),
                    static_cast<std::byte>('f'),
                };
                for (const auto* name : {"SYSTEM", "SECURITY", "SAM", "NTUSER.DAT"})
                {
                    ASSERT_TRUE(write_hive(directory_ / name, empty_hive));
                }

                const auto software_hive = create_software_hive();
                ASSERT_TRUE(write_hive(directory_ / "SOFTWARE", software_hive));
                registry_.emplace(directory_);
            }

            void TearDown() override
            {
                registry_.reset();
                std::error_code error{};
                std::filesystem::remove_all(directory_, error);
            }

            registry_manager& registry()
            {
                return *registry_;
            }

            registry_key get_key(const std::filesystem::path& path)
            {
                auto key = registry().get_key(utils::path_key{path});
                EXPECT_TRUE(key.has_value());
                return key.value_or(registry_key{});
            }

          private:
            std::filesystem::path directory_{};
            std::optional<registry_manager> registry_{};
        };
    }

    TEST_F(RegistryFileTest, ImportsUtf8BomStringsEscapesAndDword)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\ParserCase]
@="Default"
"Quoted\"Name"="Line\nQuote:\" Slash:\\"
"Number"=dword:1234abcd
)");
        import_registry_file_contents(registry(), contents, "utf8.reg");

        const auto key = get_key(uR"(\Registry\Machine\Software\ParserCase)");
        const auto default_value = registry().get_value(key, "");
        ASSERT_TRUE(default_value.has_value());
        EXPECT_EQ(default_value->as_string(), u"Default");

        const auto escaped_value = registry().get_value(key, "Quoted\"Name");
        ASSERT_TRUE(escaped_value.has_value());
        EXPECT_EQ(escaped_value->as_string(), u"Line\nQuote:\" Slash:\\");

        const auto number = registry().get_value(key, "Number");
        ASSERT_TRUE(number.has_value());
        EXPECT_EQ(number->as_dword(), 0x1234ABCDu);
    }

    TEST_F(RegistryFileTest, ImportsUtf16LittleAndBigEndian)
    {
        const auto little_endian = encode_utf16(uR"(Windows Registry Editor Version 5.00

[HKCU\Endian\Little]
"Text"="Endian"
)",
                                                false);
        const auto big_endian = encode_utf16(uR"(Windows Registry Editor Version 5.00

[HKCU\Endian\Big]
"Text"="Endian"
)",
                                             true);

        import_registry_file_contents(registry(), little_endian, "little.reg");
        import_registry_file_contents(registry(), big_endian, "big.reg");

        for (const auto* name : {u"Little", u"Big"})
        {
            const auto key = get_key(std::filesystem::path{uR"(\Registry\User\Endian)"} / name);
            const auto value = registry().get_value(key, "Text");
            ASSERT_TRUE(value.has_value());
            EXPECT_EQ(value->as_string(), u"Endian");
        }
    }

    TEST_F(RegistryFileTest, ImportsContinuedHexAndExplicitHexType)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\HexCase]
"Binary"=hex:01,02,\
  03,04,\
  05
"Multi"=hex(7):41,00,00,00
)");
        import_registry_file_contents(registry(), contents, "hex.reg");

        const auto key = get_key(uR"(\Registry\Machine\Software\HexCase)");
        const auto binary = registry().get_value(key, "Binary");
        ASSERT_TRUE(binary.has_value());
        EXPECT_EQ(binary->type, REG_BINARY);
        EXPECT_EQ(std::vector(binary->data.begin(), binary->data.end()),
                  (std::vector{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}}));

        const auto multi = registry().get_value(key, "Multi");
        ASSERT_TRUE(multi.has_value());
        EXPECT_EQ(multi->type, REG_MULTI_SZ);
        EXPECT_EQ(std::vector(multi->data.begin(), multi->data.end()),
                  (std::vector{std::byte{0x41}, std::byte{0}, std::byte{0}, std::byte{0}}));
    }

    TEST_F(RegistryFileTest, NormalizesRootAliasesAndPreservesKeyDisplayCase)
    {
        const auto contents = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00

[HKLM\Software\AliasL]
[HKCU\AliasC]
[HKU\AliasU]
[HKCR\CLSID\MiXeD"Key]
[HKCC\ConfigCase]
)");
        import_registry_file_contents(registry(), contents, "aliases.reg");

        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\Machine\Software\AliasL)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\User\AliasC)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\User\AliasU)"}).has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{uR"(\Registry\Machine\Software\Classes\CLSID\MiXeD"Key)"}).has_value());
        EXPECT_TRUE(registry()
                        .get_key(utils::path_key{uR"(\Registry\Machine\System\ControlSet001\Hardware Profiles\Current\ConfigCase)"})
                        .has_value());

        const auto clsid = get_key(uR"(\Registry\Machine\Software\Classes\CLSID)");
        ASSERT_EQ(registry().get_sub_key_count(clsid), 1u);
        const auto display_name = registry().get_sub_key_name(clsid, 0);
        ASSERT_TRUE(display_name.has_value());
        EXPECT_EQ(*display_name, "MiXeD\"Key");
    }

    TEST_F(RegistryFileTest, RejectsMalformedQuotedStringsHexTypesAndUtf16)
    {
        const auto unterminated_string = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00
[HKLM\Software\Bad]
"Value"="unterminated
)");
        const auto unterminated_type = encode_utf8_with_bom(R"(Windows Registry Editor Version 5.00
[HKLM\Software\Bad]
"Value"=hex(7:00
)");
        const std::vector odd_utf16{std::byte{0xFF}, std::byte{0xFE}, std::byte{0}};

        EXPECT_THROW(import_registry_file_contents(registry(), unterminated_string, "string.reg"), std::runtime_error);
        EXPECT_THROW(import_registry_file_contents(registry(), unterminated_type, "type.reg"), std::runtime_error);
        EXPECT_THROW(import_registry_file_contents(registry(), odd_utf16, "utf16.reg"), std::runtime_error);
    }

    TEST_F(RegistryFileTest, CreatesMissingKeyInLoadedHive)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Product)"};
        ASSERT_FALSE(registry().get_key(utils::path_key{path}).has_value());
        ASSERT_TRUE(registry().can_create_key(path));

        const auto created = registry().create_key(path);
        ASSERT_TRUE(created.has_value());
        EXPECT_TRUE(registry().get_key(utils::path_key{path}).has_value());

        const std::vector data{std::byte{1}, std::byte{2}};
        registry().set_value(*created, "Value", REG_BINARY, data);

        const auto value = registry().get_value(*created, "Value");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->type, REG_BINARY);
        EXPECT_EQ(std::vector(value->data.begin(), value->data.end()), data);
    }

    TEST_F(RegistryFileTest, CreateKeyIsIdempotent)
    {
        const std::filesystem::path path{uR"(\Registry\Machine\Software\Vendor\Once)"};

        const auto first = registry().create_key(path);
        ASSERT_TRUE(first.has_value());
        registry().set_value(*first, "Keep", REG_BINARY, std::vector{std::byte{7}});

        const auto second = registry().create_key(path);
        ASSERT_TRUE(second.has_value());
        EXPECT_TRUE(registry().get_value(*second, "Keep").has_value());
    }

    TEST_F(RegistryFileTest, RejectsKeyOutsideAnyLoadedHive)
    {
        const std::filesystem::path path{uR"(\Registry\NoSuchHive\Key)"};
        EXPECT_FALSE(registry().can_create_key(path));
        EXPECT_FALSE(registry().create_key(path).has_value());
    }
} // namespace sogen::test
