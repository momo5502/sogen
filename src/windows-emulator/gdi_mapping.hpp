#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace sogen::gdi
{
    constexpr int32_t k_map_mode_text = 1;
    constexpr int32_t k_map_mode_lometric = 2;
    constexpr int32_t k_map_mode_himetric = 3;
    constexpr int32_t k_map_mode_loenglish = 4;
    constexpr int32_t k_map_mode_hienglish = 5;
    constexpr int32_t k_map_mode_twips = 6;
    constexpr int32_t k_map_mode_isotropic = 7;
    constexpr int32_t k_map_mode_anisotropic = 8;

    struct mapping_vec
    {
        int32_t x{};
        int32_t y{};

        constexpr mapping_vec() = default;

        constexpr mapping_vec(const int32_t horizontal, const int32_t vertical)
            : x(horizontal),
              y(vertical)
        {
        }

        bool operator==(const mapping_vec&) const = default;
    };

    struct dc_mapping
    {
        int32_t map_mode{k_map_mode_text};
        mapping_vec window_org{};
        mapping_vec viewport_org{};
        mapping_vec window_ext{1, 1};
        mapping_vec viewport_ext{1, 1};

        constexpr dc_mapping() = default;

        constexpr dc_mapping(const int32_t mode, const mapping_vec& window_origin, const mapping_vec& viewport_origin,
                             const mapping_vec& window_extent, const mapping_vec& viewport_extent)
            : map_mode(mode),
              window_org(window_origin),
              viewport_org(viewport_origin),
              window_ext(window_extent),
              viewport_ext(viewport_extent)
        {
        }

        bool operator==(const dc_mapping&) const = default;
    };

    struct display_metrics
    {
        uint32_t width{};
        uint32_t height{};
        uint32_t dpi{};
    };

    namespace mapping_detail
    {
        // The kernel converts in 28.4 fixed point and lets intermediate results wrap at 32 bits.
        constexpr int64_t fixed_point_scale = 16;

        constexpr int32_t wrap_to_int32(const int64_t value)
        {
            return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
        }

        constexpr int64_t wrapping_add(const int64_t a, const int64_t b)
        {
            return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
        }

        constexpr int64_t wrapping_sub(const int64_t a, const int64_t b)
        {
            return static_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
        }

        constexpr int64_t wrapping_mul(const int64_t a, const int64_t b)
        {
            return static_cast<int64_t>(static_cast<uint64_t>(a) * static_cast<uint64_t>(b));
        }

        constexpr uint64_t unsigned_magnitude(const int64_t value)
        {
            return value < 0 ? uint64_t{0} - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
        }

        constexpr int64_t extent_magnitude(const int32_t value)
        {
            return value < 0 ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
        }

        constexpr int64_t divide_round_half_away(const int64_t numerator, const int64_t denominator)
        {
            const bool negative = (numerator < 0) != (denominator < 0);
            const auto n = unsigned_magnitude(numerator);
            const auto d = unsigned_magnitude(denominator);
            const auto rounded = n / d + (2 * (n % d) >= d ? 1 : 0);
            return static_cast<int64_t>(negative ? uint64_t{0} - rounded : rounded);
        }

        constexpr int64_t divide_round_half_up(const int64_t numerator, const int64_t denominator)
        {
            const bool negative = (numerator < 0) != (denominator < 0);
            const bool exact_half =
                2 * (unsigned_magnitude(numerator) % unsigned_magnitude(denominator)) == unsigned_magnitude(denominator);
            return wrapping_add(divide_round_half_away(numerator, denominator), negative && exact_half ? 1 : 0);
        }

        struct axis_transform
        {
            int64_t numerator{};
            int64_t denominator{};
            int64_t origin_in{};
            int64_t origin_out{};

            constexpr axis_transform(const int64_t scale_numerator, const int64_t scale_denominator, const int64_t input_origin,
                                     const int64_t output_origin)
                : numerator(scale_numerator),
                  denominator(scale_denominator),
                  origin_in(input_origin),
                  origin_out(output_origin)
            {
            }

            constexpr bool is_translation_only() const
            {
                return this->numerator == this->denominator;
            }

            constexpr int64_t translation() const
            {
                return wrapping_sub(wrapping_mul(this->origin_out, this->denominator), wrapping_mul(this->origin_in, this->numerator));
            }

            constexpr int64_t scaled_fixed_point(const int32_t value) const
            {
                const auto fixed = wrap_to_int32(wrapping_mul(value, fixed_point_scale));
                return divide_round_half_up(wrapping_mul(fixed, this->numerator), this->denominator);
            }

            constexpr int32_t plain_translated(const int32_t value) const
            {
                return wrap_to_int32(wrapping_add(wrapping_sub(value, this->origin_in), this->origin_out));
            }

            constexpr int64_t to_device_fixed_point(const int32_t value) const
            {
                if (this->is_translation_only())
                {
                    return wrapping_mul(this->plain_translated(value), fixed_point_scale);
                }

                const auto fixed_translation =
                    divide_round_half_up(wrapping_mul(this->translation(), fixed_point_scale), this->denominator);
                return wrapping_add(this->scaled_fixed_point(value), fixed_translation);
            }

            constexpr int32_t to_device(const int32_t value) const
            {
                if (this->is_translation_only())
                {
                    return this->plain_translated(value);
                }

                return wrap_to_int32(divide_round_half_up(this->to_device_fixed_point(value), fixed_point_scale));
            }

            constexpr int32_t to_logical(const int32_t value) const
            {
                if (this->is_translation_only())
                {
                    return this->plain_translated(value);
                }

                const auto whole_translation = divide_round_half_away(this->translation(), this->denominator);
                return wrap_to_int32(
                    wrapping_add(divide_round_half_away(this->scaled_fixed_point(value), fixed_point_scale), whole_translation));
            }
        };

        constexpr int32_t pixels_to_units(const uint32_t pixels, const int64_t units_per_inch, const uint32_t dpi)
        {
            return wrap_to_int32(divide_round_half_away(static_cast<int64_t>(pixels) * units_per_inch, dpi));
        }

        constexpr mapping_vec fixed_mode_window_extent(const int32_t map_mode, const display_metrics& display)
        {
            if (display.dpi == 0)
            {
                return {1, 1};
            }

            int64_t units_per_inch = 0;
            switch (map_mode)
            {
            case k_map_mode_lometric:
            case k_map_mode_isotropic:
                units_per_inch = 254;
                break;
            case k_map_mode_himetric:
                units_per_inch = 2540;
                break;
            case k_map_mode_loenglish:
                units_per_inch = 100;
                break;
            case k_map_mode_hienglish:
                units_per_inch = 1000;
                break;
            default:
                units_per_inch = 1440;
                break;
            }

            return {pixels_to_units(display.width, units_per_inch, display.dpi),
                    pixels_to_units(display.height, units_per_inch, display.dpi)};
        }

        constexpr bool scale_component(int32_t& extent, const int32_t numerator, const int32_t denominator)
        {
            if (numerator == 0 || denominator == 0)
            {
                return false;
            }

            const auto product = wrap_to_int32(static_cast<int64_t>(extent) * numerator);
            if (product == std::numeric_limits<int32_t>::min() && denominator == -1)
            {
                return false;
            }

            extent = product / denominator;
            return extent != 0;
        }
    }

    constexpr bool is_valid_map_mode(const int32_t map_mode)
    {
        return map_mode >= k_map_mode_text && map_mode <= k_map_mode_anisotropic;
    }

    constexpr bool is_fixed_map_mode(const int32_t map_mode)
    {
        return map_mode <= k_map_mode_twips;
    }

    // MM_ISOTROPIC shrinks whichever viewport extent would make a logical unit longer on one axis than on the other.
    constexpr void fit_isotropic_viewport(dc_mapping& mapping)
    {
        using mapping_detail::divide_round_half_away;
        using mapping_detail::extent_magnitude;
        using mapping_detail::wrap_to_int32;
        if (mapping.map_mode != k_map_mode_isotropic || mapping.window_ext.x == 0 || mapping.window_ext.y == 0)
        {
            return;
        }

        const auto window_x = extent_magnitude(mapping.window_ext.x);
        const auto window_y = extent_magnitude(mapping.window_ext.y);
        const auto viewport_x = extent_magnitude(mapping.viewport_ext.x);
        const auto viewport_y = extent_magnitude(mapping.viewport_ext.y);

        const auto horizontal_scale = viewport_x * window_y;
        const auto vertical_scale = viewport_y * window_x;
        if (horizontal_scale > vertical_scale)
        {
            const auto fitted = wrap_to_int32(divide_round_half_away(window_x * viewport_y, window_y));
            if (fitted != 0)
            {
                mapping.viewport_ext.x = mapping.viewport_ext.x < 0 ? wrap_to_int32(-static_cast<int64_t>(fitted)) : fitted;
            }
        }
        else if (vertical_scale > horizontal_scale)
        {
            const auto fitted = wrap_to_int32(divide_round_half_away(window_y * viewport_x, window_x));
            if (fitted != 0)
            {
                mapping.viewport_ext.y = mapping.viewport_ext.y < 0 ? wrap_to_int32(-static_cast<int64_t>(fitted)) : fitted;
            }
        }
    }

    // MM_ANISOTROPIC keeps the extents, and MM_ISOTROPIC keeps them only when it was already the map mode.
    constexpr std::optional<int32_t> set_map_mode(dc_mapping& mapping, const int32_t map_mode, const display_metrics& display)
    {
        if (!is_valid_map_mode(map_mode))
        {
            return std::nullopt;
        }

        const auto previous = mapping.map_mode;
        mapping.map_mode = map_mode;

        if (map_mode == k_map_mode_text)
        {
            mapping.window_ext = {1, 1};
            mapping.viewport_ext = {1, 1};
        }
        else if (map_mode != k_map_mode_anisotropic && (map_mode != k_map_mode_isotropic || previous != k_map_mode_isotropic))
        {
            mapping.window_ext = mapping_detail::fixed_mode_window_extent(map_mode, display);
            mapping.viewport_ext = {static_cast<int32_t>(display.width), -static_cast<int32_t>(display.height)};
        }

        fit_isotropic_viewport(mapping);
        return previous;
    }

    // The fixed map modes ignore the request but succeed. A zero factor, a zero result or INT_MIN / -1 fails and leaves the
    // extents alone. The product wraps to 32 bits before the division.
    constexpr bool scale_extent(dc_mapping& mapping, mapping_vec& extent, const int32_t x_numerator, const int32_t x_denominator,
                                const int32_t y_numerator, const int32_t y_denominator)
    {
        if (is_fixed_map_mode(mapping.map_mode))
        {
            return true;
        }

        auto scaled = extent;
        if (!mapping_detail::scale_component(scaled.x, x_numerator, x_denominator) ||
            !mapping_detail::scale_component(scaled.y, y_numerator, y_denominator))
        {
            return false;
        }

        extent = scaled;
        fit_isotropic_viewport(mapping);
        return true;
    }

    constexpr bool scale_window_extent(dc_mapping& mapping, const int32_t x_numerator, const int32_t x_denominator,
                                       const int32_t y_numerator, const int32_t y_denominator)
    {
        return scale_extent(mapping, mapping.window_ext, x_numerator, x_denominator, y_numerator, y_denominator);
    }

    constexpr bool scale_viewport_extent(dc_mapping& mapping, const int32_t x_numerator, const int32_t x_denominator,
                                         const int32_t y_numerator, const int32_t y_denominator)
    {
        return scale_extent(mapping, mapping.viewport_ext, x_numerator, x_denominator, y_numerator, y_denominator);
    }

    constexpr std::optional<mapping_vec> logical_to_device(const dc_mapping& mapping, const mapping_vec& point)
    {
        using mapping_detail::axis_transform;
        if (mapping.window_ext.x == 0 || mapping.window_ext.y == 0)
        {
            return std::nullopt;
        }

        const axis_transform horizontal{mapping.viewport_ext.x, mapping.window_ext.x, mapping.window_org.x, mapping.viewport_org.x};
        const axis_transform vertical{mapping.viewport_ext.y, mapping.window_ext.y, mapping.window_org.y, mapping.viewport_org.y};
        return mapping_vec{horizontal.to_device(point.x), vertical.to_device(point.y)};
    }

    constexpr std::optional<mapping_vec> logical_to_device_fixed_point(const dc_mapping& mapping, const mapping_vec& point)
    {
        using mapping_detail::axis_transform;
        if (mapping.window_ext.x == 0 || mapping.window_ext.y == 0)
        {
            return std::nullopt;
        }

        const axis_transform horizontal{mapping.viewport_ext.x, mapping.window_ext.x, mapping.window_org.x, mapping.viewport_org.x};
        const axis_transform vertical{mapping.viewport_ext.y, mapping.window_ext.y, mapping.window_org.y, mapping.viewport_org.y};
        return mapping_vec{mapping_detail::wrap_to_int32(horizontal.to_device_fixed_point(point.x)),
                           mapping_detail::wrap_to_int32(vertical.to_device_fixed_point(point.y))};
    }

    constexpr std::optional<mapping_vec> device_to_logical(const dc_mapping& mapping, const mapping_vec& point)
    {
        using mapping_detail::axis_transform;
        if (mapping.viewport_ext.x == 0 || mapping.viewport_ext.y == 0)
        {
            return std::nullopt;
        }

        const axis_transform horizontal{mapping.window_ext.x, mapping.viewport_ext.x, mapping.viewport_org.x, mapping.window_org.x};
        const axis_transform vertical{mapping.window_ext.y, mapping.viewport_ext.y, mapping.viewport_org.y, mapping.window_org.y};
        return mapping_vec{horizontal.to_logical(point.x), vertical.to_logical(point.y)};
    }
}
