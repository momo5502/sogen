#include "std_include.hpp"
#include "gdi_text.hpp"
#include "file_system.hpp"
#include "registry/registry_manager.hpp"
#include "windows_path.hpp"

#include <map>

namespace sogen::gdi_text
{
    std::optional<gdi_font_reference> font_source::select(const EMU_LOGFONTW& logfont, const char32_t required_character) const
    {
        try
        {
            return this->fonts.resolve(this->files, this->registry, this->system_root, logfont, required_character);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    decoded_character decode_character(const std::u16string_view text, const size_t offset)
    {
        const auto unit = text[offset];
        if (unit >= 0xD800 && unit <= 0xDBFF && offset + 1 < text.size())
        {
            const auto trail = text[offset + 1];
            if (trail >= 0xDC00 && trail <= 0xDFFF)
            {
                const auto high = static_cast<char32_t>(unit - 0xD800);
                const auto low = static_cast<char32_t>(trail - 0xDC00);
                return {0x10000 + ((high << 10) | low), 2};
            }
        }
        return {static_cast<char32_t>(unit), 1};
    }

    namespace
    {
        int32_t round_26_6(const int32_t value)
        {
            return value >= 0 ? (value + 32) >> 6 : -(((-value) + 32) >> 6);
        }

        struct glyph_entry
        {
            int32_t rounded_advance{};
            int32_t advance_26_6{};
            int32_t bearing_x_26_6{};
            int32_t width_26_6{};
        };

        using glyph_cache = std::map<gdi_font_reference, std::map<uint32_t, glyph_entry>>;

        glyph_entry cached_glyph(const font_source& source, const gdi_font_reference& font, const uint32_t index, glyph_cache& cache)
        {
            auto& entries = cache[font];
            if (const auto cached = entries.find(index); cached != entries.end())
            {
                return cached->second;
            }
            glyph_entry result{};
            try
            {
                const auto metrics = source.fonts.glyph_metrics(font, index);
                result.advance_26_6 = metrics.advance_x_26_6;
                result.rounded_advance = round_26_6(metrics.advance_x_26_6);
                result.bearing_x_26_6 = metrics.bearing_x_26_6;
                result.width_26_6 = metrics.width_26_6;
            }
            catch (...)
            {
                result.rounded_advance = static_cast<int32_t>(font.pixel_height / 2);
                result.advance_26_6 = result.rounded_advance << 6;
            }
            entries.emplace(index, result);
            return result;
        }

        uint32_t lookup_glyph(const font_source& source, const gdi_font_reference& font, const char32_t codepoint)
        {
            try
            {
                return source.fonts.glyph_index(font, codepoint);
            }
            catch (...)
            {
                return 0;
            }
        }
    }

    run layout(const font_source& source, const EMU_LOGFONTW& logfont, const std::u16string_view text, const glyph_input input,
               const std::span<const int32_t> advances, const bool advances_have_y, const int32_t fallback_advance)
    {
        run result{};
        const auto strict = source.select(logfont);
        result.physical = strict.has_value();
        glyph_cache cache{};
        if (strict)
        {
            try
            {
                const auto metrics = source.fonts.metrics(*strict);
                result.ascent = metrics.ascent;
                result.descent = metrics.descent;
                result.em_height = static_cast<int32_t>(strict->pixel_height);
            }
            catch (...)
            {
                result.physical = false;
            }
        }

        const auto advance_at = [&](const size_t unit) {
            const auto index = advances_have_y ? unit * 2 : unit;
            return index < advances.size() ? advances[index] : fallback_advance;
        };
        const auto shift_at = [&](const size_t unit) {
            const auto index = unit * 2 + 1;
            return advances_have_y && index < advances.size() ? advances[index] : 0;
        };

        for (size_t offset = 0; offset < text.size();)
        {
            const auto decoded = decode_character(text, offset);
            auto& cell = result.cells.emplace_back();
            cell.origin = result.width;
            cell.unit_count = decoded.units;

            int32_t shift = 0;
            for (size_t unit = 0; unit < decoded.units; ++unit)
            {
                const auto index = offset + unit;
                if (!advances.empty())
                {
                    cell.advance += advance_at(index);
                }
                shift += shift_at(index);
            }
            cell.shift_y = shift;

            if (input == glyph_input::indices)
            {
                if (result.physical && strict)
                {
                    cell.font = *strict;
                    cell.index = static_cast<uint16_t>(text[offset]);
                    const auto metrics = cached_glyph(source, cell.font, cell.index, cache);
                    if (advances.empty())
                    {
                        cell.advance = metrics.rounded_advance;
                    }
                    cell.advance_26_6 = metrics.advance_26_6;
                    cell.bearing_x_26_6 = metrics.bearing_x_26_6;
                    cell.width_26_6 = metrics.width_26_6;
                }
                else if (advances.empty())
                {
                    cell.missing = true;
                    cell.advance = fallback_advance;
                    cell.advance_26_6 = fallback_advance << 6;
                }
            }
            else
            {
                std::optional<gdi_font_reference> font = strict;
                uint32_t index = 0;
                if (font)
                {
                    index = lookup_glyph(source, *font, decoded.codepoint);
                }
                if (index == 0 && decoded.codepoint != 0)
                {
                    if (const auto fallback = source.select(logfont, decoded.codepoint))
                    {
                        const auto fallback_index = lookup_glyph(source, *fallback, decoded.codepoint);
                        if (fallback_index != 0)
                        {
                            font = fallback;
                            index = fallback_index;
                        }
                    }
                }
                if (font && index != 0)
                {
                    cell.font = *font;
                    cell.index = index;
                    const auto metrics = cached_glyph(source, cell.font, cell.index, cache);
                    if (advances.empty())
                    {
                        cell.advance = metrics.rounded_advance;
                    }
                    cell.advance_26_6 = metrics.advance_26_6;
                    cell.bearing_x_26_6 = metrics.bearing_x_26_6;
                    cell.width_26_6 = metrics.width_26_6;
                }
                else
                {
                    cell.missing = true;
                    if (font)
                    {
                        const auto metrics = cached_glyph(source, *font, 0, cache);
                        if (advances.empty())
                        {
                            cell.advance = metrics.rounded_advance;
                        }
                        cell.advance_26_6 = metrics.advance_26_6;
                        cell.bearing_x_26_6 = metrics.bearing_x_26_6;
                        cell.width_26_6 = metrics.width_26_6;
                    }
                    else
                    {
                        cell.advance = advances.empty() ? fallback_advance : cell.advance;
                        cell.advance_26_6 = fallback_advance << 6;
                    }
                }
            }

            result.width += cell.advance;
            offset += decoded.units;
        }
        return result;
    }

    measure_result measure(const font_source& source, const std::optional<EMU_LOGFONTW>& logfont, const std::u16string_view text,
                           const glyph_input input, const std::span<const int32_t> advances, const bool advances_have_y,
                           const int32_t fallback_advance, const measure_visitor& visit)
    {
        measure_result result{};
        if (!logfont)
        {
            int64_t origin = 0;
            for (size_t i = 0; i < text.size(); ++i)
            {
                const auto advance = advances.empty() ? fallback_advance : advances[advances_have_y ? i * 2 : i];
                if (visit)
                {
                    visit({.unit_begin = i, .unit_count = 1, .origin = origin, .advance = advance, .a = 0, .b = advance, .c = 0});
                }

                origin += advance;
            }

            result.width = origin;
            return result;
        }

        const auto elements_per_unit = advances_have_y ? 2 : 1;

        // Layout in bounded blocks so a huge request never holds one cell per unit; a surrogate pair split across a
        // block edge decodes as two lone surrogates, which only affects supplementary characters past 0x4000 units.
        constexpr size_t block_units = 0x4000;
        for (size_t begin = 0; begin < text.size(); begin += block_units)
        {
            const auto unit_count = std::min(block_units, text.size() - begin);
            auto block_advances = advances;
            if (!advances.empty())
            {
                const auto element_begin = std::min(begin * elements_per_unit, advances.size());
                block_advances = advances.subspan(element_begin);
            }

            const auto run =
                layout(source, *logfont, text.substr(begin, unit_count), input, block_advances, advances_have_y, fallback_advance);
            if (begin == 0)
            {
                result.ascent = run.ascent;
                result.descent = run.descent;
                result.physical = run.physical;
            }

            size_t unit_begin = begin;
            for (const auto& cell : run.cells)
            {
                if (visit)
                {
                    auto a = round_26_6(cell.bearing_x_26_6);
                    auto b = round_26_6(cell.width_26_6);
                    const auto rounded_advance = round_26_6(cell.advance_26_6);
                    if (!run.physical)
                    {
                        a = 0;
                        b = cell.advance;
                    }

                    visit({.unit_begin = unit_begin,
                           .unit_count = cell.unit_count,
                           .origin = result.width + cell.origin,
                           .advance = cell.advance,
                           .a = a,
                           .b = b,
                           .c = rounded_advance - a - b});
                }

                unit_begin += cell.unit_count;
            }

            result.width += run.width;
        }

        return result;
    }
}
