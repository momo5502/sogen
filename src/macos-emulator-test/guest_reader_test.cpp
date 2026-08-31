#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <trace/macos_guest_reader.hpp>

#include <string_view>

namespace
{
    constexpr uint64_t data_base = 0x300000000ULL;

    TEST(MacosGuestReader, ReadsANulTerminatedString)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        constexpr std::string_view text{"/usr/lib/dyld"};
        emu->memory.write_memory(data_base, text.data(), text.size() + 1);

        const auto result = sogen::read_bounded_guest_string(emu->memory, data_base, 256);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->text, "/usr/lib/dyld");
        EXPECT_FALSE(result->truncated);
    }

    TEST(MacosGuestReader, StopsAtTheLimitAndReportsTruncation)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const std::string text(600, 'a');
        emu->memory.write_memory(data_base, text.data(), text.size() + 1);

        const auto result = sogen::read_bounded_guest_string(emu->memory, data_base, 256);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->text.size(), 256u);
        EXPECT_TRUE(result->truncated);
    }

    TEST(MacosGuestReader, UnmappedPointerYieldsNulloptRatherThanThrowing)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_NO_THROW({
            const auto result = sogen::read_bounded_guest_string(emu->memory, 0x700000000ULL, 256);
            EXPECT_FALSE(result.has_value());
        });
    }

    TEST(MacosGuestReader, NullPointerYieldsNullopt)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_FALSE(sogen::read_bounded_guest_string(emu->memory, 0, 256).has_value());
        EXPECT_FALSE(sogen::read_bounded_guest_bytes(emu->memory, 0, 16).has_value());
    }

    TEST(MacosGuestReader, ReturnsTheReadablePrefixWhenAStringRunsOffTheMapping)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(data_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto tail = data_base + sogen::MACOS_PAGE_SIZE - 4;
        emu->memory.write_memory(tail, "abcd", 4);

        const auto result = sogen::read_bounded_guest_string(emu->memory, tail, 256);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->text, "abcd");
        EXPECT_TRUE(result->truncated);
    }

    TEST(MacosGuestReader, EscapesControlCharacters)
    {
        EXPECT_EQ(sogen::escape_trace_text("a\nb\tc"), "a\\nb\\tc");
        EXPECT_EQ(sogen::escape_trace_text(std::string_view("\x01\x7f", 2)), "\\x01\\x7f");
        EXPECT_EQ(sogen::escape_trace_text("say \"hi\""), "say \\\"hi\\\"");
        EXPECT_EQ(sogen::escape_trace_text("back\\slash"), "back\\\\slash");
    }

    TEST(MacosGuestReader, QuotesAndMarksTruncation)
    {
        EXPECT_EQ(sogen::quote_trace_text({"Hello, sogen!\n", false}), "\"Hello, sogen!\\n\"");
        EXPECT_EQ(sogen::quote_trace_text({"abc", true}), "\"abc\"...");
    }

    TEST(MacosGuestReader, PreviewsBytesAsHexAndRefusesAnOutOfRangeWindow)
    {
        const std::array<uint8_t, 4> bytes{0x00, 0x01, 0xFE, 0xFF};
        EXPECT_EQ(sogen::format_byte_preview(bytes, 0, 4, 4), "00 01 fe ff");
        EXPECT_EQ(sogen::format_byte_preview(bytes, 0, 2, 9), "00 01 ... (9 bytes)");
        EXPECT_EQ(sogen::format_byte_preview(bytes, 5, 1, 4), "<unreadable>");
        EXPECT_EQ(sogen::format_byte_preview(bytes, 2, 4, 4), "<unreadable>");

        // offset + length wraps for both of these, so a bounds test written that way accepts the window
        // and then walks off the span. Only the subtraction form refuses them.
        EXPECT_EQ(sogen::format_byte_preview(bytes, SIZE_MAX, 2, 4), "<unreadable>");
        EXPECT_EQ(sogen::format_byte_preview(bytes, 2, SIZE_MAX, 4), "<unreadable>");
    }

    TEST(MacosGuestReader, FormatsHexWithAnUnpaddedLowercaseBody)
    {
        EXPECT_EQ(sogen::format_hex(0), "0x0");
        EXPECT_EQ(sogen::format_hex(0x1234ABCDULL), "0x1234abcd");
    }
}
