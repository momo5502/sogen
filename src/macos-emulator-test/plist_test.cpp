#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <plist.hpp>

namespace
{
    std::vector<std::byte> bytes_of(const std::string_view text)
    {
        std::vector<std::byte> data{};
        data.reserve(text.size());
        for (const char ch : text)
        {
            data.push_back(static_cast<std::byte>(ch));
        }
        return data;
    }

    void append_bytes(std::vector<std::byte>& out, const std::initializer_list<int> values)
    {
        for (const int value : values)
        {
            out.push_back(static_cast<std::byte>(value));
        }
    }

    void append_text(std::vector<std::byte>& out, const std::string_view text)
    {
        for (const char ch : text)
        {
            out.push_back(static_cast<std::byte>(ch));
        }
    }

    void append_be64(std::vector<std::byte>& out, const uint64_t value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }

    void append_trailer(std::vector<std::byte>& out, const uint64_t object_count, const uint64_t top_object, const uint64_t table_offset)
    {
        out.resize(out.size() + 6, std::byte{0});
        append_bytes(out, {0x01, 0x01});
        append_be64(out, object_count);
        append_be64(out, top_object);
        append_be64(out, table_offset);
    }

    // Produced on macOS 26.6.1 with `plutil -convert binary1` from a plist authored for this test
    // (CFBundleIdentifier / CFBundlePackageType / CFBundleExecutable). Note the key order differs from
    // the source XML: a binary plist stores keys in its own order, so lookup must scan, not index.
    constexpr std::array<uint8_t, 149> BINARY_INFO_PLIST{
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xd3, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x5f, 0x10, 0x12, 0x43, 0x46, 0x42, 0x75,
        0x6e, 0x64, 0x6c, 0x65, 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x66, 0x69, 0x65, 0x72, 0x5f, 0x10, 0x13, 0x43, 0x46, 0x42, 0x75, 0x6e,
        0x64, 0x6c, 0x65, 0x50, 0x61, 0x63, 0x6b, 0x61, 0x67, 0x65, 0x54, 0x79, 0x70, 0x65, 0x5f, 0x10, 0x12, 0x43, 0x46, 0x42, 0x75, 0x6e,
        0x64, 0x6c, 0x65, 0x45, 0x78, 0x65, 0x63, 0x75, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x5f, 0x10, 0x10, 0x64, 0x65, 0x76, 0x2e, 0x73, 0x6f,
        0x67, 0x65, 0x6e, 0x2e, 0x57, 0x69, 0x64, 0x67, 0x65, 0x74, 0x54, 0x41, 0x50, 0x50, 0x4c, 0x56, 0x57, 0x69, 0x64, 0x67, 0x65, 0x74,
        0x08, 0x0f, 0x24, 0x3a, 0x4f, 0x62, 0x67, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6e,
    };

    std::span<const std::byte> binary_plist()
    {
        return std::as_bytes(std::span{BINARY_INFO_PLIST});
    }

    constexpr std::string_view XML_INFO_PLIST = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>Widget</string>
	<key>CFBundleIdentifier</key>
	<string>dev.sogen.Widget</string>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleExecutable</key>
			<string>NotTheRealOne</string>
		</dict>
	</array>
	<key>CFBundleDisplayName</key>
	<string>Widget &amp; Co &lt;beta&gt;</string>
	<key>LSMinimumSystemVersion</key>
	<string></string>
	<key>LSUIElement</key>
	<true/>
</dict>
</plist>
)";

    TEST(Plist, ReadsAStringFromAnXmlPlist)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_EQ(sogen::plist_top_level_string(data, "CFBundleExecutable"), "Widget");
        EXPECT_EQ(sogen::plist_top_level_string(data, "CFBundleIdentifier"), "dev.sogen.Widget");
    }

    TEST(Plist, IgnoresKeysNestedInsideOtherDictionaries)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_EQ(sogen::plist_top_level_string(data, "CFBundleExecutable"), "Widget");
    }

    TEST(Plist, DecodesXmlEntities)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_EQ(sogen::plist_top_level_string(data, "CFBundleDisplayName"), "Widget & Co <beta>");
    }

    TEST(Plist, ReturnsAnEmptyStringForAnEmptyElement)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        const auto value = sogen::plist_top_level_string(data, "LSMinimumSystemVersion");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, "");
    }

    TEST(Plist, ReturnsNothingWhenTheValueIsNotAString)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "LSUIElement").has_value());
    }

    TEST(Plist, ReturnsNothingForAnAbsentKey)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleVersion").has_value());
    }

    TEST(Plist, ReadsAStringFromABinaryPlist)
    {
        EXPECT_EQ(sogen::plist_top_level_string(binary_plist(), "CFBundleExecutable"), "Widget");
        EXPECT_EQ(sogen::plist_top_level_string(binary_plist(), "CFBundleIdentifier"), "dev.sogen.Widget");
        EXPECT_EQ(sogen::plist_top_level_string(binary_plist(), "CFBundlePackageType"), "APPL");
    }

    TEST(Plist, ReturnsNothingForAnAbsentBinaryKey)
    {
        EXPECT_FALSE(sogen::plist_top_level_string(binary_plist(), "CFBundleVersion").has_value());
    }

    TEST(Plist, RejectsATruncatedBinaryPlist)
    {
        for (const size_t length : {size_t{8}, size_t{16}, size_t{40}, size_t{100}, size_t{148}})
        {
            const auto truncated = binary_plist().first(length);
            EXPECT_FALSE(sogen::plist_top_level_string(truncated, "CFBundleExecutable").has_value()) << "truncated to " << length;
        }
    }

    TEST(Plist, RejectsABinaryPlistWithAnOffsetTableOutsideTheFile)
    {
        std::vector<std::byte> data(BINARY_INFO_PLIST.size());
        std::memcpy(data.data(), BINARY_INFO_PLIST.data(), data.size());
        data[data.size() - 1] = static_cast<std::byte>(0xF0);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleExecutable").has_value());
    }

    TEST(Plist, RejectsABinaryPlistWhoseTopObjectIsOutOfRange)
    {
        std::vector<std::byte> data(BINARY_INFO_PLIST.size());
        std::memcpy(data.data(), BINARY_INFO_PLIST.data(), data.size());
        data[data.size() - 32 + 23] = static_cast<std::byte>(0x40);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleExecutable").has_value());
    }

    TEST(Plist, RejectsEmptyAndOversizedInput)
    {
        EXPECT_FALSE(sogen::plist_top_level_string({}, "CFBundleExecutable").has_value());

        const std::vector<std::byte> oversized(sogen::MAX_PLIST_SIZE + 1, std::byte{'a'});
        EXPECT_FALSE(sogen::plist_top_level_string(oversized, "CFBundleExecutable").has_value());
    }

    TEST(Plist, RejectsGarbage)
    {
        const auto data = bytes_of("this is not a property list at all");
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleExecutable").has_value());
    }

    constexpr std::string_view XML_NESTED_KEY_FIRST = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>CFBundleDocumentTypes</key>
	<array>
		<dict>
			<key>CFBundleExecutable</key>
			<string>NotTheRealOne</string>
			<key>CFBundleIdentifier</key>
			<string>dev.sogen.Impostor</string>
		</dict>
	</array>
	<key>CFBundleExecutable</key>
	<string>Widget</string>
</dict>
</plist>
)";

    TEST(Plist, SkipsANestedKeyThatPrecedesTheRealTopLevelOne)
    {
        const auto data = bytes_of(XML_NESTED_KEY_FIRST);
        EXPECT_EQ(sogen::plist_top_level_string(data, "CFBundleExecutable"), "Widget");
    }

    TEST(Plist, DoesNotFindAKeyThatOnlyExistsInsideANestedDictionary)
    {
        const auto data = bytes_of(XML_NESTED_KEY_FIRST);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleIdentifier").has_value());
    }

    std::vector<std::byte> utf16_binary_plist()
    {
        std::vector<std::byte> data{};
        append_text(data, "bplist00");
        append_bytes(data, {0xD1, 0x01, 0x02});
        append_bytes(data, {0x54, 'N', 'a', 'm', 'e'});
        append_bytes(data, {0x62, 0x00, 0x41, 0x00, 0xE9});
        append_bytes(data, {0x08, 0x0B, 0x10});
        append_trailer(data, 3, 0, 21);
        return data;
    }

    TEST(Plist, ReadsAUtf16BinaryStringAsUtf8)
    {
        EXPECT_EQ(sogen::plist_top_level_string(utf16_binary_plist(), "Name"), "A\xc3\xa9");
    }

    TEST(Plist, RejectsAnUnpairedSurrogateInAUtf16BinaryString)
    {
        auto data = utf16_binary_plist();
        data[17] = std::byte{0xD8};
        data[18] = std::byte{0x00};
        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    TEST(Plist, RejectsAnAsciiStringWhoseDeclaredLengthExceedsTheFile)
    {
        std::vector<std::byte> data{};
        append_text(data, "bplist00");
        append_bytes(data, {0xD1, 0x01, 0x02});
        append_bytes(data, {0x54, 'N', 'a', 'm', 'e'});
        append_bytes(data, {0x5F, 0x13, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        append_bytes(data, {0x08, 0x0B, 0x10});
        append_trailer(data, 3, 0, 26);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    TEST(Plist, RejectsADictionaryWithAnAbsurdEntryCount)
    {
        std::vector<std::byte> data{};
        append_text(data, "bplist00");
        append_bytes(data, {0xDF, 0x13, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        append_bytes(data, {0x08});
        append_trailer(data, 1, 0, 18);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    TEST(Plist, RejectsAnOffsetTableEntryPointingAtTheOffsetTable)
    {
        auto data = utf16_binary_plist();
        data[21] = std::byte{21};
        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    TEST(Plist, RejectsADictionaryWhoseValueRefersBackToTheDictionary)
    {
        auto data = utf16_binary_plist();
        data[10] = std::byte{0x00};
        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    // A long-form length field sits *after* the object marker, so an object placed in the file's last
    // bytes has its length bytes outside the file. The trailer occupies the final 32 bytes and its own
    // fields are range-checked, which leaves exactly one reachable placement: a one-byte length field
    // whose marker doubles as the low bytes of table_offset. That forces table_offset to 0x5F10, hence
    // the deliberately large buffer.
    TEST(Plist, RejectsALongFormLengthFieldThatStartsAtTheEndOfTheFile)
    {
        constexpr size_t TABLE_OFFSET = 0x5F10;
        constexpr size_t TOTAL_SIZE = TABLE_OFFSET + 12 + 32;
        constexpr size_t TRAILER = TOTAL_SIZE - 32;

        std::vector<std::byte> data(TOTAL_SIZE, std::byte{0});
        const char* magic = "bplist00";
        for (size_t i = 0; i < 8; ++i)
        {
            data[i] = static_cast<std::byte>(magic[i]);
        }

        data[8] = std::byte{0xD1};
        data[9] = std::byte{0x01};
        data[10] = std::byte{0x02};

        const char* name = "Name";
        data[11] = std::byte{0x54};
        for (size_t i = 0; i < 4; ++i)
        {
            data[12 + i] = static_cast<std::byte>(name[i]);
        }

        const auto put_be32 = [&](const size_t at, const uint32_t value) {
            for (size_t i = 0; i < 4; ++i)
            {
                data[at + i] = static_cast<std::byte>((value >> (8 * (3 - i))) & 0xFFU);
            }
        };

        put_be32(TABLE_OFFSET, 8);
        put_be32(TABLE_OFFSET + 4, 11);
        put_be32(TABLE_OFFSET + 8, static_cast<uint32_t>(TOTAL_SIZE - 2));

        data[TRAILER + 6] = std::byte{0x04};
        data[TRAILER + 7] = std::byte{0x01};
        data[TRAILER + 15] = std::byte{0x03};

        data[TOTAL_SIZE - 2] = std::byte{0x5F};
        data[TOTAL_SIZE - 1] = std::byte{0x10};

        ASSERT_EQ(static_cast<uint32_t>(data[TRAILER + 30]) << 8 | static_cast<uint32_t>(data[TRAILER + 31]), TABLE_OFFSET);

        EXPECT_FALSE(sogen::plist_top_level_string(data, "Name").has_value());
    }

    TEST(Plist, RejectsEveryTruncationOfABinaryPlist)
    {
        const auto full = binary_plist();
        for (size_t length = 0; length < full.size(); ++length)
        {
            EXPECT_FALSE(sogen::plist_top_level_string(full.first(length), "CFBundleExecutable").has_value()) << "truncated to " << length;
        }
        EXPECT_EQ(sogen::plist_top_level_string(full, "CFBundleExecutable"), "Widget");
    }

    TEST(Plist, SurvivesEverySingleByteMutationOfABinaryPlist)
    {
        constexpr std::array<int, 12> POISON{0x00, 0x01, 0x08, 0x0F, 0x10, 0x13, 0x5F, 0x7F, 0x80, 0xD0, 0xDF, 0xFF};

        for (size_t index = 0; index < BINARY_INFO_PLIST.size(); ++index)
        {
            for (const int poison : POISON)
            {
                std::vector<std::byte> data(BINARY_INFO_PLIST.size());
                std::memcpy(data.data(), BINARY_INFO_PLIST.data(), data.size());
                data[index] = static_cast<std::byte>(poison);

                const auto value = sogen::plist_top_level_string(data, "CFBundleExecutable");
                if (value.has_value())
                {
                    EXPECT_LE(value->size(), data.size()) << "index " << index << " poison " << poison;
                }
            }
        }
    }

    TEST(Plist, SurvivesRandomMutationsOfABinaryPlist)
    {
        std::mt19937 rng{0x50D1C7u}; // NOLINT(bugprone-random-generator-seed,cert-msc51-cpp): a fixed seed keeps failures reproducible

        for (int iteration = 0; iteration < 20000; ++iteration)
        {
            std::vector<std::byte> data(BINARY_INFO_PLIST.size());
            std::memcpy(data.data(), BINARY_INFO_PLIST.data(), data.size());

            const auto edits = rng() % 6 + 1;
            for (uint32_t edit = 0; edit < edits; ++edit)
            {
                data[rng() % data.size()] = static_cast<std::byte>(rng() & 0xFFU);
            }

            if (rng() % 4 == 0)
            {
                data.resize(rng() % data.size());
            }

            const auto value = sogen::plist_top_level_string(data, "CFBundleExecutable");
            if (value.has_value())
            {
                EXPECT_LE(value->size(), BINARY_INFO_PLIST.size());
            }
        }
    }

    TEST(Plist, SurvivesRandomMutationsOfAnXmlPlist)
    {
        const auto original = bytes_of(XML_INFO_PLIST);
        std::mt19937 rng{0x58D1C7u}; // NOLINT(bugprone-random-generator-seed,cert-msc51-cpp): a fixed seed keeps failures reproducible

        for (int iteration = 0; iteration < 20000; ++iteration)
        {
            auto data = original;

            const auto edits = rng() % 6 + 1;
            for (uint32_t edit = 0; edit < edits; ++edit)
            {
                data[rng() % data.size()] = static_cast<std::byte>(rng() & 0xFFU);
            }

            if (rng() % 4 == 0)
            {
                data.resize(rng() % data.size());
            }

            const auto value = sogen::plist_top_level_string(data, "CFBundleExecutable");
            if (value.has_value())
            {
                EXPECT_LE(value->size(), original.size());
            }
        }
    }

    TEST(Plist, TerminatesOnAdversarialXml)
    {
        for (const std::string_view text :
             {"<", "<dict>", "<dict><!-- never closed", "<dict><key>unclosed", "</dict>", "<dict><key>a</key><string>b", "<<<<<<<<",
              "<dict/><dict/><dict/>", "<?xml", "<!", "<dict><key></key>"})
        {
            const auto data = bytes_of(text);
            EXPECT_FALSE(sogen::plist_top_level_string(data, "a").has_value()) << text;
        }
    }

    TEST(Plist, TerminatesOnDeeplyNestedXmlWithoutRecursing)
    {
        std::string text{};
        for (int i = 0; i < 200000; ++i)
        {
            text += "<dict>";
        }
        text += "<key>CFBundleExecutable</key><string>Widget</string>";

        const auto data = bytes_of(text);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "CFBundleExecutable").has_value());
    }

    TEST(Plist, RejectsUnknownAndMalformedXmlEntities)
    {
        for (const std::string_view entity : {"&nosuch;", "&", "&#;", "&#xZZ;", "&#999999999;"})
        {
            const auto text = std::string{"<plist><dict><key>k</key><string>"} + std::string{entity} + "</string></dict></plist>";
            const auto data = bytes_of(text);
            EXPECT_FALSE(sogen::plist_top_level_string(data, "k").has_value()) << entity;
        }
    }

    TEST(Plist, RejectsAnEmptyKey)
    {
        const auto data = bytes_of(XML_INFO_PLIST);
        EXPECT_FALSE(sogen::plist_top_level_string(data, "").has_value());
    }

    TEST(Plist, RejectsAnOversizedPlistThatWouldOtherwiseParse)
    {
        const std::string document = "<plist><dict><key>CFBundleExecutable</key><string>Widget</string></dict></plist>";

        std::string at_the_limit = document;
        at_the_limit.append(sogen::MAX_PLIST_SIZE - document.size(), ' ');
        ASSERT_EQ(at_the_limit.size(), sogen::MAX_PLIST_SIZE);
        EXPECT_EQ(sogen::plist_top_level_string(bytes_of(at_the_limit), "CFBundleExecutable"), "Widget");

        const std::string past_the_limit = at_the_limit + " ";
        EXPECT_FALSE(sogen::plist_top_level_string(bytes_of(past_the_limit), "CFBundleExecutable").has_value());
    }

    TEST(Plist, ReturnsAnEmptyStringForASelfClosingStringElement)
    {
        const auto data = bytes_of("<plist><dict><key>k</key><string/></dict></plist>");
        const auto value = sogen::plist_top_level_string(data, "k");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, "");
    }
}
