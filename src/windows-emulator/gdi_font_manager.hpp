#pragma once

#include "windows_path.hpp"
#include <platform/platform.hpp>

#include <compare>
#include <memory>
#include <optional>
#include <vector>

namespace sogen
{
    class file_system;
    class registry_manager;

    struct gdi_font_reference
    {
        uint32_t file{};
        uint32_t face{};
        uint32_t pixel_width{};
        uint32_t pixel_height{};
        bool embolden{};
        bool oblique{};

        auto operator<=>(const gdi_font_reference&) const = default;
    };

    struct gdi_font_metrics
    {
        int32_t ascent{};
        int32_t descent{};
        int32_t height{};
        int32_t average_width{};
        int32_t maximum_width{};
        bool fixed_pitch{};
    };

    struct gdi_glyph_metrics
    {
        int32_t advance_x_26_6{};
        int32_t advance_y_26_6{};
        int32_t bearing_x_26_6{};
        int32_t bearing_y_26_6{};
        int32_t width_26_6{};
        int32_t height_26_6{};
    };

    struct gdi_glyph_bitmap
    {
        gdi_glyph_metrics metrics{};
        int32_t left{};
        int32_t top{};
        uint32_t width{};
        uint32_t height{};
        std::vector<uint8_t> coverage{};
    };

    class gdi_font_manager
    {
      public:
        gdi_font_manager();
        ~gdi_font_manager();
        gdi_font_manager(gdi_font_manager&&) noexcept;
        gdi_font_manager& operator=(gdi_font_manager&&) noexcept;
        gdi_font_manager(const gdi_font_manager&) = delete;
        gdi_font_manager& operator=(const gdi_font_manager&) = delete;

        std::optional<gdi_font_reference> resolve(file_system& files, registry_manager& registry, const windows_path& system_root,
                                                  const EMU_LOGFONTW& request, char32_t required_character = 0);
        uint32_t glyph_index(const gdi_font_reference& font, char32_t character);
        gdi_font_metrics metrics(const gdi_font_reference& font);
        gdi_glyph_metrics glyph_metrics(const gdi_font_reference& font, uint32_t index);
        std::shared_ptr<const gdi_glyph_bitmap> rasterize(const gdi_font_reference& font, uint32_t index);
        std::u16string family_name(const gdi_font_reference& font);

        void clear();
        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        struct implementation;
        std::unique_ptr<implementation> impl_{};
        implementation& get();
    };
}
