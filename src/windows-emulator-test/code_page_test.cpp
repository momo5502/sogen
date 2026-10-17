#include <gtest/gtest.h>
#include <code_page.hpp>

#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr size_t forward_offset = 14 * sizeof(uint16_t);
        constexpr size_t reverse_offset = 273 * sizeof(uint16_t);

        void write_word(std::vector<std::byte>& data, const size_t offset, const uint16_t value)
        {
            data.at(offset) = static_cast<std::byte>(value & 0xFF);
            data.at(offset + 1) = static_cast<std::byte>(value >> 8);
        }

        std::vector<std::byte> make_code_page()
        {
            std::vector<std::byte> result(reverse_offset + 0x10000, std::byte{'?'});
            write_word(result, 0, 13);
            write_word(result, 4, 1);
            write_word(result, 26, 259);
            for (size_t i = 0; i < 256; ++i)
            {
                write_word(result, forward_offset + i * sizeof(uint16_t), static_cast<uint16_t>(i));
                result.at(reverse_offset + i) = static_cast<std::byte>(i);
            }
            return result;
        }
    }

    TEST(CodePageTest, PreservesSingleByteCharacters)
    {
        const auto page = make_code_page();
        const auto tables = build_ansi_oem_tables(page, page);
        ASSERT_TRUE(tables);
        for (size_t i = 0; i < 256; ++i)
        {
            EXPECT_EQ(i, tables->ansi_to_oem.at(i));
            EXPECT_EQ(i, tables->oem_to_ansi.at(i));
        }
    }

    TEST(CodePageTest, ConvertsThroughUnicodeAndUsesBestFitTable)
    {
        auto ansi = make_code_page();
        auto oem = make_code_page();
        write_word(ansi, forward_offset + 0xE9 * sizeof(uint16_t), 0xE9);
        write_word(oem, forward_offset + 0x82 * sizeof(uint16_t), 0xE9);
        oem.at(reverse_offset + 0xE9) = std::byte{0x82};
        ansi.at(reverse_offset + 0xE9) = std::byte{0xE9};
        write_word(ansi, forward_offset + 0x80 * sizeof(uint16_t), 0x20AC);
        oem.at(reverse_offset + 0x20AC) = std::byte{'E'};
        const auto tables = build_ansi_oem_tables(ansi, oem);
        ASSERT_TRUE(tables);
        EXPECT_EQ(0x82, tables->ansi_to_oem.at(0xE9));
        EXPECT_EQ(0xE9, tables->oem_to_ansi.at(0x82));
        EXPECT_EQ('E', tables->ansi_to_oem.at(0x80));
    }

    TEST(CodePageTest, RejectsMissingAndTruncatedFiles)
    {
        const auto page = make_code_page();
        EXPECT_FALSE(build_ansi_oem_tables({}, page));
        EXPECT_FALSE(build_ansi_oem_tables(page, {}));
        for (const size_t size : {size_t{25}, size_t{27}, forward_offset + 511, reverse_offset + 0xFFFF})
        {
            EXPECT_FALSE(build_ansi_oem_tables(std::span{page}.first(size), page));
        }
    }

    TEST(CodePageTest, RejectsOffsetsInsideHeaderAndOutsideFile)
    {
        const auto valid = make_code_page();
        auto invalid = valid;
        write_word(invalid, 0, 1);
        EXPECT_FALSE(build_ansi_oem_tables(invalid, valid));
        write_word(invalid, 0, 0xFFFF);
        EXPECT_FALSE(build_ansi_oem_tables(invalid, valid));
        invalid = valid;
        write_word(invalid, 26, 0xFFFF);
        EXPECT_FALSE(build_ansi_oem_tables(valid, invalid));
    }

    TEST(CodePageTest, DoesNotTreatMultibyteCodePagesAsSingleByte)
    {
        const auto valid = make_code_page();
        auto multibyte = valid;
        write_word(multibyte, 4, 2);
        EXPECT_FALSE(build_ansi_oem_tables(valid, multibyte));
        write_word(multibyte, 4, 4);
        EXPECT_FALSE(build_ansi_oem_tables(multibyte, valid));
    }
}
