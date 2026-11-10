#include <gtest/gtest.h>
#include <code_page.hpp>
#include "code_page_test_utils.hpp"
#include <utils/serialization.hpp>

#include <algorithm>
#include <vector>

#if defined(OS_WINDOWS) && defined(_WIN64)
#include <platform/platform.hpp>
#include <utils/io.hpp>
#endif

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

        std::vector<std::byte> make_code_page(const bool glyph_table = false)
        {
            const auto reverse = reverse_offset + (glyph_table ? 512 : 0);
            std::vector<std::byte> result(reverse + 0x10000);
            std::fill(result.begin() + reverse, result.end(), std::byte{'?'});
            write_word(result, 0, 13);
            write_word(result, 2, 437);
            write_word(result, 4, 1);
            write_word(result, 26, static_cast<uint16_t>((reverse - 26) / 2 - 1));
            write_word(result, forward_offset + 512, glyph_table ? 256 : 0);
            for (size_t i = 0; i < 256; ++i)
            {
                write_word(result, forward_offset + i * sizeof(uint16_t), static_cast<uint16_t>(i));
                result.at(reverse + i) = static_cast<std::byte>(i);
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

    TEST(CodePageTest, DecodesAndEncodesSingleByteNlsWithOptionalGlyphTable)
    {
        for (const bool glyph_table : {false, true})
        {
            auto data = make_code_page(glyph_table);
            write_word(data, forward_offset + 0x82 * 2, 0x00E9);
            data.at(reverse_offset + (glyph_table ? 512 : 0) + 0x00E9) = std::byte{0x82};
            const code_page page{std::move(data)};
            EXPECT_EQ(page.decode("A\x82"), u"A\x00E9");
            EXPECT_EQ(page.encode(u"A\x00E9\x2603"), "A\x82?");
        }
    }

    TEST(CodePageTest, KeepsCp1252ExactMappingInsteadOfNlsBestFit)
    {
        auto data = make_code_page();
        write_word(data, 2, 1252);
        data.at(reverse_offset + 0x0100) = std::byte{'A'};
        const code_page page{std::move(data)};
        EXPECT_EQ(page.decode("\x80\x93\x94"), u"\x20AC\x201C\x201D");
        EXPECT_EQ(page.encode(u"\x0100\x4E2D\xD800"), "???");
        EXPECT_EQ(code_page{}.encode(u"\x0100\x4E2D"), "??");
    }

    TEST(CodePageTest, ConvertsMixedSingleAndDoubleByteCharacters)
    {
        const code_page page{make_dbcs_code_page()};
        const std::string bytes = "A\xD6\xD0\xCE\xC4\x80";
        const std::u16string text = u"A\x4E2D\x6587\x20AC";
        EXPECT_EQ(page.id(), 936);
        EXPECT_EQ(page.maximum_character_size(), 2);
        EXPECT_EQ(page.decode(bytes), text);
        EXPECT_EQ(page.encode(text), bytes);
        EXPECT_EQ(page.encoded_size(text), 6);
        EXPECT_EQ(page.encode(u"\x2603"), "?");
    }

    TEST(CodePageTest, MatchesRtlDefaultAndIncompleteDoubleByteSequences)
    {
        const code_page page{make_dbcs_code_page()};
        EXPECT_EQ(page.decode("\xD6 "), u"?");
        EXPECT_EQ(page.decode("A\xD6"), (std::u16string{u'A', u'\0'}));
        EXPECT_EQ(page.decode(std::string{"\xD6\0", 2}), u"?");
        EXPECT_EQ(page.decode(std::string{"A\0B", 3}), (std::u16string{u'A', u'\0', u'B'}));
    }

    TEST(CodePageTest, DoesNotSplitDoubleByteCharactersAtOutputLimit)
    {
        const code_page page{make_dbcs_code_page()};
        const std::u16string text = u"\x4E2D"
                                    u"A\x6587";
        EXPECT_EQ(page.encode(text, 0), "");
        EXPECT_EQ(page.encode(text, 1), "");
        EXPECT_EQ(page.encode(text, 2), "\xD6\xD0");
        EXPECT_EQ(page.encode(text, 3), "\xD6\xD0"
                                        "A");
        EXPECT_EQ(page.encode(text, 4), "\xD6\xD0"
                                        "A");
        EXPECT_EQ(page.encode(text, 5), "\xD6\xD0"
                                        "A\xCE\xC4");
    }

    TEST(CodePageTest, RejectsTruncatedAndOutOfRangeDoubleByteTables)
    {
        auto data = make_dbcs_code_page();
        data.pop_back();
        EXPECT_THROW((code_page{data}), std::invalid_argument);
        data = make_dbcs_code_page();
        write_code_page_word(data, 544 + 0xD6 * 2, 0xFFFF);
        EXPECT_THROW((code_page{data}), std::invalid_argument);
        data = make_dbcs_code_page();
        write_code_page_word(data, 544 + 0xD6 * 2, 1);
        EXPECT_THROW((code_page{data}), std::invalid_argument);
        data = make_dbcs_code_page();
        data.at(16) = std::byte{0xD7};
        EXPECT_THROW((code_page{data}), std::invalid_argument);
    }

    TEST(CodePageTest, RestoresOwnedDoubleByteDataWithoutOriginalObject)
    {
        utils::buffer_serializer saved{};
        {
            const code_page page{make_dbcs_code_page()};
            saved.write(page);
        }
        code_page restored{};
        utils::buffer_deserializer input{saved};
        input.read(restored);
        EXPECT_EQ(restored.id(), 936);
        EXPECT_EQ(restored.decode("\xD6\xD0\xCE\xC4"), u"\x4E2D\x6587");
        EXPECT_EQ(restored.encode(u"\x4E2D\x6587"), "\xD6\xD0\xCE\xC4");
    }

#if defined(OS_WINDOWS) && defined(_WIN64)
    TEST(CodePageTest, InstalledNlsMatchesNativeRtlConversions)
    {
        const auto module = GetModuleHandleW(L"ntdll.dll");
        ASSERT_NE(module, nullptr);
        const auto initialize = reinterpret_cast<void(NTAPI*)(const void*, CPTABLEINFO*)>(GetProcAddress(module, "RtlInitCodePageTable"));
        const auto decode = reinterpret_cast<NTSTATUS(NTAPI*)(const CPTABLEINFO*, void*, ULONG, ULONG*, const void*, ULONG)>(
            GetProcAddress(module, "RtlCustomCPToUnicodeN"));
        const auto encode = reinterpret_cast<NTSTATUS(NTAPI*)(const CPTABLEINFO*, void*, ULONG, ULONG*, const void*, ULONG)>(
            GetProcAddress(module, "RtlUnicodeToCustomCPN"));
        ASSERT_NE(initialize, nullptr);
        ASSERT_NE(decode, nullptr);
        ASSERT_NE(encode, nullptr);
        std::array<wchar_t, MAX_PATH> directory{};
        const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        ASSERT_GT(length, 0u);
        ASSERT_LT(length, directory.size());

        for (const uint16_t id : {437, 850, 932, 936, 950, 1251, 1252})
        {
            SCOPED_TRACE(id);
            auto data = utils::io::read_file(std::filesystem::path{directory.data()} / ("c_" + std::to_string(id) + ".nls"));
            ASSERT_FALSE(data.empty());
            const code_page page{std::move(data)};
            CPTABLEINFO native{};
            initialize(page.data().data(), &native);
            EXPECT_EQ(page.maximum_character_size(), native.MaximumCharacterSize);

            std::string input{};
            for (size_t i = 0; i < 256; ++i)
            {
                input.push_back(static_cast<char>(i));
            }
            input += "\x81\\\xD6\xD0\xCE\xC4\x80\xD6";
            std::u16string expected(input.size(), u'\0');
            ULONG written{};
            ASSERT_GE(decode(&native, expected.data(), static_cast<ULONG>(expected.size() * sizeof(char16_t)), &written, input.data(),
                             static_cast<ULONG>(input.size())),
                      0);
            expected.resize(written / sizeof(char16_t));
            EXPECT_EQ(page.decode(input), expected);

            if (id == 1252)
            {
                continue;
            }
            const std::u16string text{u'A', u'\0', char16_t{0x20AC}, char16_t{0x4E2D}, char16_t{0x6587}, char16_t{0x0100}, char16_t{0xD800},
                                      u'Z'};
            for (const ULONG capacity : {0u, 1u, 2u, 3u, 8u, 16u})
            {
                std::string bytes(capacity, '\0');
                written = 0;
                (void)encode(&native, bytes.data(), capacity, &written, text.data(), static_cast<ULONG>(text.size() * sizeof(char16_t)));
                ASSERT_LE(written, capacity);
                bytes.resize(written);
                EXPECT_EQ(page.encode(text, capacity), bytes);
                if (capacity == 16)
                {
                    EXPECT_EQ(page.encoded_size(text), bytes.size());
                }
            }
        }
    }
#endif
}
