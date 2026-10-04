#include "../std_include.hpp"
#include "macos_display_state.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace sogen
{
    namespace
    {
        void put_u8(std::vector<uint8_t>& out, const uint8_t value)
        {
            out.push_back(value);
        }

        void put_bytes(std::vector<uint8_t>& out, const std::span<const uint8_t> bytes)
        {
            out.insert(out.end(), bytes.begin(), bytes.end());
        }

        void put_u32(std::vector<uint8_t>& out, const uint32_t value)
        {
            for (size_t i = 0; i < sizeof(value); ++i)
            {
                out.push_back(static_cast<uint8_t>(value >> (i * 8)));
            }
        }

        void put_u64(std::vector<uint8_t>& out, const uint64_t value)
        {
            for (size_t i = 0; i < sizeof(value); ++i)
            {
                out.push_back(static_cast<uint8_t>(value >> (i * 8)));
            }
        }

        void put_f32(std::vector<uint8_t>& out, const float value)
        {
            put_u32(out, std::bit_cast<uint32_t>(value));
        }

        void put_f64(std::vector<uint8_t>& out, const double value)
        {
            put_u64(out, std::bit_cast<uint64_t>(value));
        }

        void put_rect(std::vector<uint8_t>& out, const double x, const double y, const double width, const double height)
        {
            put_f64(out, x);
            put_f64(out, y);
            put_f64(out, width);
            put_f64(out, height);
        }

        // Every field of a mode is a fixed-width scalar except this one: 32 characters plus a terminator
        // saying where each component sits in a pixel. The measured built-in panel of the reference host
        // reads "--RRRRRRRRRRGGGGGGGGGGBBBBBBBBBB"; sogen's backing store is 8 bits per component, which
        // is IOKit's 32-bit direct form.
        constexpr std::string_view pixel_encoding_32 = "--------RRRRRRRRGGGGGGGGBBBBBBBB";
        constexpr size_t pixel_encoding_bytes = 33;

        constexpr uint32_t bits_per_pixel = 32;
        constexpr uint32_t bits_per_sample = 8;
        constexpr uint32_t samples_per_pixel = 3;

        // Measured on the built-in panel of a 25G76 host. Each is a word the client stores and sogen has
        // no configuration behind: a mode's kind, the two flag words a scaled mode carries (the native
        // mode of the same panel sets 0x02000000 in both on top of these), and the three constants of a
        // display record that do not vary with geometry.
        constexpr uint32_t mode_kind = 4;
        constexpr uint32_t mode_flags = 1;
        constexpr uint32_t mode_extended_flags = 3;
        constexpr uint32_t display_record_kind = 6;
        constexpr uint32_t display_word_0x1c = 1;
        constexpr uint32_t display_word_0x28 = 20;

        // An Apple-panel vendor/model pair. Nothing sogen answers depends on it; a display with a zeroed
        // one is the shape the reference host's placeholder displays have, and those never host a window.
        constexpr uint32_t display_vendor = 1552;
        constexpr uint32_t display_model = 41041;

        // rec+0xec..+0xff, the capability bytes SLSDisplayGetCapabilities reports. Taken from the
        // built-in panel because a display that demonstrably hosts windows is the only measured set.
        constexpr std::array<uint8_t, 20> display_capabilities{
            0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        };

        // The 16 bytes that follow the "1" inside a mode and the current-mode index after the mode list.
        // The client copies both into scratch it never reads back, so their role stayed undecoded; a
        // count of zero omits the mode's copy entirely on the wire.
        constexpr std::array<uint8_t, 16> trailing_entry{
            0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };

        uint32_t dots_per_inch(const double pixels, const double inches)
        {
            if (inches <= 0.0)
            {
                return 0;
            }

            return static_cast<uint32_t>(std::lround(pixels / inches));
        }

        void append_mode(std::vector<uint8_t>& out, const uint32_t index, const macos_display_description& display)
        {
            const auto pixel_width = static_cast<uint32_t>(std::lround(display.width * display.scale));
            const auto pixel_height = static_cast<uint32_t>(std::lround(display.height * display.scale));

            put_u32(out, mode_kind);
            put_u32(out, index);
            put_u32(out, index);
            put_u32(out, mode_flags);
            put_u32(out, mode_extended_flags);
            put_f32(out, static_cast<float>(display.refresh_hz));
            put_u32(out, pixel_width);
            put_u32(out, pixel_height);
            put_u32(out, static_cast<uint32_t>(std::lround(display.width)));
            put_u32(out, static_cast<uint32_t>(std::lround(display.height)));
            put_u32(out, pixel_width * (bits_per_pixel / 8));
            put_u32(out, dots_per_inch(pixel_width, display.width / MACOS_DISPLAY_DPI));
            put_u32(out, dots_per_inch(pixel_height, display.height / MACOS_DISPLAY_DPI));
            put_f32(out, static_cast<float>(display.scale));

            std::array<uint8_t, pixel_encoding_bytes> encoding{};
            std::memcpy(encoding.data(), pixel_encoding_32.data(), pixel_encoding_32.size());
            put_bytes(out, encoding);

            put_u32(out, bits_per_pixel / 4);
            put_u32(out, bits_per_pixel);
            put_u32(out, bits_per_sample);
            put_u32(out, samples_per_pixel);
            put_u8(out, 0);
            put_u32(out, 1);
            put_bytes(out, trailing_entry);
            put_u8(out, 1);
            put_f32(out, static_cast<float>(display.refresh_hz));
            put_u8(out, 0);
        }

        void append_display(std::vector<uint8_t>& out, const macos_display_description& display)
        {
            put_u32(out, display_record_kind);
            put_u32(out, display.id);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, display_word_0x1c);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, display_word_0x28);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, 0);
            put_u32(out, display_vendor);
            put_u32(out, display_model);
            put_u32(out, display.id);

            put_bytes(out, macos_display_uuid(display.id));

            put_rect(out, display.x, display.y, display.width, display.height);
            put_rect(out, display.x, display.y, display.width, display.height);
            put_rect(out, 0.0, 0.0, display.width * display.scale, display.height * display.scale);
            put_f64(out, display.width / MACOS_DISPLAY_DPI);
            put_f64(out, display.height / MACOS_DISPLAY_DPI);

            put_bytes(out, display_capabilities);
            put_u32(out, 0);
            put_u32(out, 0);

            put_u32(out, 1);
            append_mode(out, 0, display);

            put_u32(out, 0);
            put_bytes(out, trailing_entry);
            put_rect(out, 0.0, 0.0, 0.0, 0.0);
            put_u32(out, display.menubar_height);
            put_u8(out, 0);
            put_u8(out, 0);
            put_u64(out, 0);
            put_u8(out, 0);
        }
    }

    std::array<uint8_t, 16> macos_display_uuid(const uint32_t display_id)
    {
        return {0x53,
                0x4f,
                0x47,
                0x45,
                0x4e,
                0x00,
                0x40,
                0x00,
                0x80,
                0x00,
                0x00,
                0x00,
                static_cast<uint8_t>(display_id >> 24),
                static_cast<uint8_t>(display_id >> 16),
                static_cast<uint8_t>(display_id >> 8),
                static_cast<uint8_t>(display_id)};
    }

    std::vector<uint8_t> macos_build_display_system_state(const std::span<const macos_display_description> displays,
                                                          const uint64_t generation)
    {
        std::vector<uint8_t> out{};

        put_u32(out, 0);
        put_u64(out, generation);
        put_u32(out, 0x58);
        put_u32(out, 0x14);
        put_u32(out, 1);
        put_u32(out, 0);
        put_u32(out, 0);
        put_u32(out, static_cast<uint32_t>(displays.size()));

        for (const auto& display : displays)
        {
            append_display(out, display);
        }

        return out;
    }
}
