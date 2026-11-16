#pragma once

#include "gdi_font_manager.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace sogen
{
    class file_system;
    class registry_manager;
    class windows_path;

    namespace gdi_text
    {
        enum class glyph_input : uint8_t
        {
            characters,
            indices,
        };

        struct font_source
        {
            gdi_font_manager& fonts;
            file_system& files;
            registry_manager& registry;
            const windows_path& system_root;

            std::optional<gdi_font_reference> select(const EMU_LOGFONTW& logfont, char32_t required_character = 0) const;
        };

        struct cell
        {
            gdi_font_reference font{};
            uint32_t index{};
            bool missing{};
            int32_t advance{};
            int32_t advance_26_6{};
            int32_t bearing_x_26_6{};
            int32_t width_26_6{};
            int32_t shift_y{};
            int64_t origin{};
            uint32_t unit_count{1};
        };

        struct run
        {
            int32_t ascent{};
            int32_t descent{};
            int32_t em_height{};
            int64_t width{};
            bool physical{};
            std::vector<cell> cells{};
        };

        run layout(const font_source& source, const EMU_LOGFONTW& logfont, std::u16string_view text, glyph_input input,
                   std::span<const int32_t> advances, bool advances_have_y, int32_t fallback_advance);

        struct decoded_character
        {
            char32_t codepoint{};
            uint32_t units{1};
        };

        decoded_character decode_character(std::u16string_view text, size_t offset);

        // Block-based measurement for large counts: visits every cell (amortized over its UTF-16 units) without
        // holding one cell per unit. `a`/`b`/`c` are the ABC split of the cell advance.
        struct measured_cell
        {
            size_t unit_begin{};
            uint32_t unit_count{1};
            int64_t origin{};
            int32_t advance{};
            int32_t a{};
            int32_t b{};
            int32_t c{};
        };

        struct measure_result
        {
            int64_t width{};
            int32_t ascent{};
            int32_t descent{};
            bool physical{};
        };

        using measure_visitor = std::function<void(const measured_cell&)>;

        measure_result measure(const font_source& source, const std::optional<EMU_LOGFONTW>& logfont, std::u16string_view text,
                               glyph_input input, std::span<const int32_t> advances, bool advances_have_y, int32_t fallback_advance,
                               const measure_visitor& visit);
    }
}
