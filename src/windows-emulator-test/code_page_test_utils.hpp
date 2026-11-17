#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sogen::test
{
    inline void write_code_page_word(std::vector<std::byte>& data, const size_t offset, const uint16_t value)
    {
        data.at(offset) = static_cast<std::byte>(value & 0xFF);
        data.at(offset + 1) = static_cast<std::byte>(value >> 8);
    }

    inline std::vector<std::byte> make_sbcs_code_page(const uint16_t id = 437, const bool glyph_table = false)
    {
        constexpr size_t forward = 28;
        const size_t reverse = 546 + (glyph_table ? 512 : 0);
        std::vector<std::byte> data(reverse + 0x10000);
        std::fill(data.begin() + reverse, data.end(), std::byte{'?'});
        write_code_page_word(data, 0, 13);
        write_code_page_word(data, 2, id);
        write_code_page_word(data, 4, 1);
        for (size_t offset = 6; offset <= 12; offset += 2)
        {
            write_code_page_word(data, offset, '?');
        }
        write_code_page_word(data, 26, static_cast<uint16_t>((reverse - 26) / 2 - 1));
        write_code_page_word(data, forward + 512, glyph_table ? 256 : 0);
        for (size_t i = 0; i < 256; ++i)
        {
            write_code_page_word(data, forward + i * 2, static_cast<uint16_t>(i));
            if (glyph_table)
            {
                write_code_page_word(data, forward + 514 + i * 2, static_cast<uint16_t>(i));
            }
            data.at(reverse + i) = static_cast<std::byte>(i);
        }
        return data;
    }

    inline std::vector<std::byte> make_dbcs_code_page()
    {
        constexpr size_t forward = 28;
        constexpr size_t offsets = 544;
        constexpr size_t reverse = 2082;
        std::vector<std::byte> data(reverse + 0x20000);
        write_code_page_word(data, 0, 13);
        write_code_page_word(data, 2, 936);
        write_code_page_word(data, 4, 2);
        for (size_t offset = 6; offset <= 12; offset += 2)
        {
            write_code_page_word(data, offset, '?');
        }
        data.at(14) = std::byte{0xCE};
        data.at(15) = std::byte{0xCE};
        data.at(16) = std::byte{0xD6};
        data.at(17) = std::byte{0xD6};
        write_code_page_word(data, 26, 1027);
        write_code_page_word(data, 542, 2);
        write_code_page_word(data, offsets + 0xCE * 2, 256);
        write_code_page_word(data, offsets + 0xD6 * 2, 512);
        for (size_t i = 0; i < 256; ++i)
        {
            write_code_page_word(data, forward + i * 2, static_cast<uint16_t>(i < 128 ? i : '?'));
            write_code_page_word(data, offsets + (256 + i) * 2, '?');
            write_code_page_word(data, offsets + (512 + i) * 2, '?');
        }
        for (size_t i = 0; i < 0x10000; ++i)
        {
            write_code_page_word(data, reverse + i * 2, static_cast<uint16_t>(i < 128 ? i : '?'));
        }
        write_code_page_word(data, forward + 0x80 * 2, 0x20AC);
        write_code_page_word(data, offsets + (256 + 0xC4) * 2, 0x6587);
        write_code_page_word(data, offsets + (512 + 0xD0) * 2, 0x4E2D);
        write_code_page_word(data, reverse + 0x20AC * 2, 0x80);
        write_code_page_word(data, reverse + 0x4E2D * 2, 0xD6D0);
        write_code_page_word(data, reverse + 0x6587 * 2, 0xCEC4);
        return data;
    }
}
