#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sogen::test::font_fixture
{
    using bytes = std::vector<std::byte>;

    inline void word(bytes& data, const size_t offset, const uint16_t value)
    {
        data.at(offset) = static_cast<std::byte>(value >> 8);
        data.at(offset + 1) = static_cast<std::byte>(value & 255);
    }

    inline void dword(bytes& data, const size_t offset, const uint32_t value)
    {
        word(data, offset, static_cast<uint16_t>(value >> 16));
        word(data, offset + 2, static_cast<uint16_t>(value));
    }

    inline uint32_t read_dword(const bytes& data, const size_t offset)
    {
        uint32_t result{};
        for (size_t i = 0; i < 4; ++i)
        {
            result = (result << 8) | std::to_integer<uint32_t>(data.at(offset + i));
        }
        return result;
    }

    inline void append_word(bytes& data, const uint16_t value)
    {
        const auto offset = data.size();
        data.resize(offset + 2);
        word(data, offset, value);
    }

    inline void align(bytes& data)
    {
        while (data.size() % 4 != 0)
        {
            data.push_back(std::byte{});
        }
    }

    inline uint32_t checksum(const bytes& data)
    {
        uint32_t result{};
        for (size_t i = 0; i < data.size(); i += 4)
        {
            result += read_dword(data, i);
        }
        return result;
    }

    inline bytes outline(const std::vector<std::pair<int16_t, int16_t>>& points)
    {
        bytes data(10);
        word(data, 0, 1);
        int16_t left = points.front().first;
        int16_t right = left;
        int16_t bottom = points.front().second;
        int16_t top = bottom;
        for (const auto& [x, y] : points)
        {
            left = std::min(left, x);
            right = std::max(right, x);
            bottom = std::min(bottom, y);
            top = std::max(top, y);
        }
        word(data, 2, static_cast<uint16_t>(left));
        word(data, 4, static_cast<uint16_t>(bottom));
        word(data, 6, static_cast<uint16_t>(right));
        word(data, 8, static_cast<uint16_t>(top));
        append_word(data, static_cast<uint16_t>(points.size() - 1));
        append_word(data, 0);
        data.insert(data.end(), points.size(), std::byte{1});
        for (const auto axis : {false, true})
        {
            int16_t previous{};
            for (const auto& point : points)
            {
                const auto value = axis ? point.second : point.first;
                append_word(data, static_cast<uint16_t>(value - previous));
                previous = value;
            }
        }
        align(data);
        return data;
    }

    // 按 OpenType SFNT/cmap/glyf 格式构造独立测试数据，不包含任何已发布字体的字节。
    inline bytes make_font(const std::u16string& family = u"Sogen Fixture", const bool chinese = true, const bool bold = false)
    {
        std::map<std::string, bytes> tables{};
        auto& head = tables["head"];
        head.resize(54);
        dword(head, 0, 0x10000);
        dword(head, 4, 0x10000);
        dword(head, 12, 0x5F0F3CF5);
        word(head, 18, 1024);
        word(head, 40, 1024);
        word(head, 42, 768);
        word(head, 44, bold ? 1 : 0);
        word(head, 46, 8);
        word(head, 48, 2);
        word(head, 50, 1);

        auto& hhea = tables["hhea"];
        hhea.resize(36);
        dword(hhea, 0, 0x10000);
        word(hhea, 4, 1024);
        word(hhea, 6, static_cast<uint16_t>(-256));
        word(hhea, 10, 1024);
        word(hhea, 16, 1024);
        word(hhea, 18, 1);
        word(hhea, 34, 4);
        auto& maxp = tables["maxp"];
        maxp.resize(32);
        dword(maxp, 0, 0x10000);
        word(maxp, 4, 4);
        word(maxp, 6, 4);
        word(maxp, 8, 1);
        word(maxp, 14, 2);

        const std::array outlines{
            outline({{0, 0}, {0, 768}, {512, 768}, {512, 0}}),
            outline({{0, 0}, {256, 768}, {512, 0}}),
            outline({{64, 0}, {64, 768}, {960, 768}, {960, 0}}),
            outline({{64, 0}, {512, 768}, {960, 0}}),
        };
        auto& glyf = tables["glyf"];
        auto& loca = tables["loca"];
        loca.resize(20);
        auto& hmtx = tables["hmtx"];
        hmtx.resize(16);
        for (size_t i = 0; i < outlines.size(); ++i)
        {
            dword(loca, i * 4, static_cast<uint32_t>(glyf.size()));
            glyf.insert(glyf.end(), outlines[i].begin(), outlines[i].end());
            word(hmtx, i * 4, static_cast<uint16_t>(i < 2 ? 512 : 1024));
            word(hmtx, i * 4 + 2, static_cast<uint16_t>(i < 2 ? 0 : 64));
        }
        dword(loca, 16, static_cast<uint32_t>(glyf.size()));

        const std::vector<std::pair<uint16_t, uint16_t>> mapping =
            chinese ? std::vector<std::pair<uint16_t, uint16_t>>{{0x41, 1}, {0x4E2D, 2}, {0x6587, 3}, {0xFFFF, 0}}
                    : std::vector<std::pair<uint16_t, uint16_t>>{{0x41, 1}, {0xFFFF, 0}};
        const auto segments = mapping.size();
        auto& cmap = tables["cmap"];
        cmap.resize(12 + 16 + segments * 8);
        word(cmap, 2, 1);
        word(cmap, 4, 3);
        word(cmap, 6, 1);
        dword(cmap, 8, 12);
        word(cmap, 12, 4);
        word(cmap, 14, static_cast<uint16_t>(16 + segments * 8));
        word(cmap, 18, static_cast<uint16_t>(segments * 2));
        word(cmap, 20, static_cast<uint16_t>(segments * 2));
        word(cmap, 22, static_cast<uint16_t>(chinese ? 2 : 1));
        for (size_t i = 0; i < segments; ++i)
        {
            word(cmap, 26 + i * 2, mapping[i].first);
            word(cmap, 28 + segments * 2 + i * 2, mapping[i].first);
            word(cmap, 28 + segments * 4 + i * 2, static_cast<uint16_t>(mapping[i].second - mapping[i].first));
        }

        auto& os2 = tables["OS/2"];
        os2.resize(78);
        word(os2, 2, 512);
        word(os2, 4, bold ? 700 : 400);
        word(os2, 6, 5);
        dword(os2, 42, 1);
        dword(os2, 46, chinese ? 1u << 27 : 0);
        word(os2, 62, bold ? 0x20 : 0x40);
        word(os2, 64, 0x41);
        word(os2, 66, chinese ? 0x6587 : 0x41);
        word(os2, 68, 1024);
        word(os2, 70, static_cast<uint16_t>(-256));
        word(os2, 74, 1024);
        word(os2, 76, 256);
        auto& post = tables["post"];
        post.resize(32);
        dword(post, 0, 0x30000);

        const std::array<std::pair<uint16_t, std::u16string>, 4> names{
            {{1, family}, {2, bold ? u"Bold" : u"Regular"}, {4, bold ? family + u" Bold" : family}, {6, u"SogenFixture"}}};
        auto& name = tables["name"];
        const size_t string_offset = 6 + 12 * names.size();
        name.resize(string_offset);
        word(name, 2, static_cast<uint16_t>(names.size()));
        word(name, 4, static_cast<uint16_t>(string_offset));
        for (size_t i = 0; i < names.size(); ++i)
        {
            const auto record = 6 + i * 12;
            word(name, record, 3);
            word(name, record + 2, 1);
            word(name, record + 4, 0x409);
            word(name, record + 6, names[i].first);
            word(name, record + 8, static_cast<uint16_t>(names[i].second.size() * 2));
            word(name, record + 10, static_cast<uint16_t>(name.size() - string_offset));
            for (const auto ch : names[i].second)
            {
                append_word(name, static_cast<uint16_t>(ch));
            }
        }

        bytes result(12 + 16 * tables.size());
        dword(result, 0, 0x10000);
        word(result, 4, static_cast<uint16_t>(tables.size()));
        word(result, 6, 128);
        word(result, 8, 3);
        word(result, 10, static_cast<uint16_t>(tables.size() * 16 - 128));
        size_t record = 12;
        size_t head_offset = 0;
        for (auto& [tag, data] : tables)
        {
            const auto length = data.size();
            align(data);
            for (size_t i = 0; i < 4; ++i)
            {
                result[record + i] = static_cast<std::byte>(tag[i]);
            }
            dword(result, record + 4, checksum(data));
            dword(result, record + 8, static_cast<uint32_t>(result.size()));
            dword(result, record + 12, static_cast<uint32_t>(length));
            if (tag == "head")
            {
                head_offset = result.size();
            }
            result.insert(result.end(), data.begin(), data.end());
            record += 16;
        }
        dword(result, head_offset + 8, 0xB1B0AFBAu - checksum(result));
        return result;
    }

    inline bytes make_collection(std::vector<bytes> fonts)
    {
        bytes result(12 + fonts.size() * 4);
        dword(result, 0, 0x74746366);
        dword(result, 4, 0x10000);
        dword(result, 8, static_cast<uint32_t>(fonts.size()));
        for (size_t i = 0; i < fonts.size(); ++i)
        {
            align(result);
            const auto base = static_cast<uint32_t>(result.size());
            dword(result, 12 + i * 4, base);
            auto& font = fonts[i];
            const auto count = (std::to_integer<uint16_t>(font[4]) << 8) | std::to_integer<uint16_t>(font[5]);
            for (size_t record = 12; record < 12 + static_cast<size_t>(count) * 16; record += 16)
            {
                const auto offset = read_dword(font, record + 8);
                if (font[record] == std::byte{'h'} && font[record + 1] == std::byte{'e'} && font[record + 2] == std::byte{'a'} &&
                    font[record + 3] == std::byte{'d'})
                {
                    dword(font, offset + 8, 0);
                }
                dword(font, record + 8, base + offset);
            }
            result.insert(result.end(), font.begin(), font.end());
        }
        return result;
    }
}
