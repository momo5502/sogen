#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace sogen
{
    struct display_mode
    {
        uint32_t width{};
        uint32_t height{};

        bool operator==(const display_mode&) const = default;

        bool fits_within(const std::optional<display_mode>& limit) const
        {
            return !limit || (this->width <= limit->width && this->height <= limit->height);
        }
    };

    // The single virtual display advertises a list of common modes so callers (e.g. DXVK's mode enumeration, which
    // the game uses to validate available resolutions) see real entries.
    inline constexpr std::array<display_mode, 8> display_modes = {{
        {.width = 640, .height = 480},
        {.width = 800, .height = 600},
        {.width = 1024, .height = 768},
        {.width = 1280, .height = 720},
        {.width = 1280, .height = 1024},
        {.width = 1600, .height = 900},
        {.width = 1680, .height = 1050},
        {.width = 1920, .height = 1080},
    }};

    inline constexpr display_mode default_display_mode = display_modes.back();

    inline std::string to_string(const display_mode& mode)
    {
        return std::to_string(mode.width) + "x" + std::to_string(mode.height);
    }

    // Parses "<width>x<height>" naming one of display_modes.
    inline std::optional<display_mode> parse_display_mode(const std::string_view text)
    {
        const auto separator = text.find('x');
        if (separator == std::string_view::npos)
        {
            return std::nullopt;
        }

        const auto parse_number = [](const std::string_view part, uint32_t& value) {
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
            return error == std::errc{} && end == part.data() + part.size();
        };

        display_mode mode{};
        if (!parse_number(text.substr(0, separator), mode.width) || !parse_number(text.substr(separator + 1), mode.height))
        {
            return std::nullopt;
        }

        if (std::ranges::find(display_modes, mode) == display_modes.end())
        {
            return std::nullopt;
        }

        return mode;
    }
}
