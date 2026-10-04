#include "../std_include.hpp"
#include "macos_sdf_field.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace sogen
{
    namespace
    {
        constexpr double SIMILARITY_TOLERANCE = 1e-9;

        // One 8-bit level of coverage. A smooth-min that moves the field by less than this cannot have
        // moved a painted pixel, so it is not a place where the section 5.2 residual could show.
        constexpr double COVERAGE_LEVEL = 1.0 / 255.0;

        bool finite(const double value)
        {
            return std::isfinite(value);
        }

        bool finite_rect(const macos_layer_rect& rect)
        {
            return finite(rect.x) && finite(rect.y) && finite(rect.width) && finite(rect.height);
        }

        bool finite_affine(const macos_layer_affine& transform)
        {
            return finite(transform.a) && finite(transform.b) && finite(transform.c) && finite(transform.d) && finite(transform.tx) &&
                   finite(transform.ty);
        }

        double clamp01(const double value)
        {
            return finite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
        }

        // A distance field maps through a rotation, a reflection or a uniform scale by scaling its
        // values; under a shear or a non-uniform scale it stops being a distance field at all, and the
        // coverage ramp and effectOffset of sections 4 and 5.4 lose their point-space meaning.
        std::optional<double> similarity_scale(const macos_layer_affine& transform)
        {
            if (!finite_affine(transform))
            {
                return std::nullopt;
            }

            const auto column0 = transform.a * transform.a + transform.b * transform.b;
            const auto column1 = transform.c * transform.c + transform.d * transform.d;
            const auto reference = std::max({column0, column1, 1.0});

            if (std::abs(column0 - column1) > SIMILARITY_TOLERANCE * reference)
            {
                return std::nullopt;
            }

            if (std::abs(transform.a * transform.c + transform.b * transform.d) > SIMILARITY_TOLERANCE * reference)
            {
                return std::nullopt;
            }

            const auto scale = std::sqrt(column0);
            return scale > 0.0 ? std::optional{scale} : std::nullopt;
        }

        void note(macos_sdf_stats& stats, const macos_sdf_diagnostic_kind kind, std::string name, std::string detail)
        {
            stats.diagnostics.push_back({kind, std::move(name), std::move(detail)});
        }

        macos_layer_rect inflated(const macos_layer_rect& rect, const double margin)
        {
            return {rect.x - margin, rect.y - margin, rect.width + 2.0 * margin, rect.height + 2.0 * margin};
        }

        macos_layer_rect mapped_bounding_box(const macos_layer_affine& transform, const macos_layer_rect& rect)
        {
            const std::array corners{
                transform.apply({rect.x, rect.y}),
                transform.apply({rect.x + rect.width, rect.y}),
                transform.apply({rect.x, rect.y + rect.height}),
                transform.apply({rect.x + rect.width, rect.y + rect.height}),
            };

            auto min_x = corners[0].x;
            auto max_x = corners[0].x;
            auto min_y = corners[0].y;
            auto max_y = corners[0].y;

            for (const auto& corner : corners)
            {
                min_x = std::min(min_x, corner.x);
                max_x = std::max(max_x, corner.x);
                min_y = std::min(min_y, corner.y);
                max_y = std::max(max_y, corner.y);
            }

            return {min_x, min_y, max_x - min_x, max_y - min_y};
        }

        macos_layer_rect united(const macos_layer_rect& first, const macos_layer_rect& second)
        {
            const auto min_x = std::min(first.x, second.x);
            const auto min_y = std::min(first.y, second.y);
            const auto max_x = std::max(first.x + first.width, second.x + second.width);
            const auto max_y = std::max(first.y + first.height, second.y + second.height);
            return {min_x, min_y, max_x - min_x, max_y - min_y};
        }

        struct device_box
        {
            int32_t x0{};
            int32_t y0{};
            int32_t x1{};
            int32_t y1{};

            bool empty() const
            {
                return this->x1 <= this->x0 || this->y1 <= this->y0;
            }
        };

        struct raster_plan
        {
            macos_layer_affine device_to_container{};
            double scale{1.0};
            device_box box{};
        };

        device_box clipped_box(const macos_layer_rect& rect, const int32_t width, const int32_t height)
        {
            if (!finite_rect(rect))
            {
                return {};
            }

            const auto low_x = std::floor(rect.x);
            const auto low_y = std::floor(rect.y);
            const auto high_x = std::ceil(rect.x + rect.width);
            const auto high_y = std::ceil(rect.y + rect.height);

            device_box box{};
            box.x0 = static_cast<int32_t>(std::clamp(low_x, 0.0, static_cast<double>(width)));
            box.y0 = static_cast<int32_t>(std::clamp(low_y, 0.0, static_cast<double>(height)));
            box.x1 = static_cast<int32_t>(std::clamp(high_x, 0.0, static_cast<double>(width)));
            box.y1 = static_cast<int32_t>(std::clamp(high_y, 0.0, static_cast<double>(height)));
            return box;
        }

        std::optional<raster_plan> plan_raster(const macos_sdf_prepared_field& field, const macos_layer_affine& container_to_device,
                                               const int32_t width, const int32_t height, macos_sdf_stats& stats)
        {
            const auto scale = similarity_scale(container_to_device);
            const auto inverse = container_to_device.inverse();

            if (!scale || !inverse)
            {
                note(stats, macos_sdf_diagnostic_kind::unmodelled, "containerToDevice",
                     "the container's mapping to device pixels is not an invertible similarity, so the "
                     "one-device-pixel coverage ramp of section 4 has no single width; nothing was drawn");
                return std::nullopt;
            }

            if (!field.has_extent)
            {
                return std::nullopt;
            }

            raster_plan plan{};
            plan.device_to_container = *inverse;
            plan.scale = *scale;

            const auto margin = 0.5 / plan.scale + field.smoothness + std::max(0.0, -field.effect_offset) + 1.0;
            plan.box = clipped_box(mapped_bounding_box(container_to_device, inflated(field.extent, margin)), width, height);
            return plan;
        }

        void note_smoothness_residual(macos_sdf_stats& stats, const macos_sdf_prepared_field& field)
        {
            if (field.smoothness <= MACOS_SDF_SMOOTHNESS_MEASURED_EXACT || stats.smooth_blend_pixels == 0)
            {
                return;
            }

            note(stats, macos_sdf_diagnostic_kind::approximated, "smoothness",
                 "the polynomial smooth-min of section 5.2 is measured exact only at 2 and at the gap "
                 "configurations; it over-predicts the junction of two tangent circles by 8% at 8 and 25% at 32");
        }
    }

    std::optional<macos_sdf_mode> macos_sdf_mode_from_name(const std::string_view name)
    {
        static const std::map<std::string_view, macos_sdf_mode> names{
            {"bounds", macos_sdf_mode::bounds},
            {"contents", macos_sdf_mode::contents},
        };

        const auto found = names.find(name);
        return found == names.end() ? std::nullopt : std::optional{found->second};
    }

    std::optional<macos_sdf_operation> macos_sdf_operation_from_name(const std::string_view name)
    {
        static const std::map<std::string_view, macos_sdf_operation> names{
            {"union", macos_sdf_operation::unite},
            {"subtraction", macos_sdf_operation::subtract},
        };

        const auto found = names.find(name);
        return found == names.end() ? std::nullopt : std::optional{found->second};
    }

    std::optional<macos_sdf_effect_kind> macos_sdf_effect_kind_from_class_name(const std::string_view class_name)
    {
        static const std::map<std::string_view, macos_sdf_effect_kind> names{
            {"CASDFFillEffect", macos_sdf_effect_kind::fill},
            {"CASDFGradientEffect", macos_sdf_effect_kind::gradient},
            {"CASDFGradientContourEffect", macos_sdf_effect_kind::gradient_contour},
            {"CASDFShadowEffect", macos_sdf_effect_kind::shadow},
            {"CASDFGlassHighlightEffect", macos_sdf_effect_kind::glass_highlight},
            {"CASDFGlassDisplacementEffect", macos_sdf_effect_kind::glass_displacement},
            {"CASDFKeyFillHighlightEffect", macos_sdf_effect_kind::key_fill_highlight},
            {"CASDFOutputEffect", macos_sdf_effect_kind::output},
            {"CASDFVisualizationEffect", macos_sdf_effect_kind::visualization},
        };

        const auto found = names.find(class_name);
        return found == names.end() ? std::nullopt : std::optional{found->second};
    }

    std::string_view macos_sdf_effect_kind_name(const macos_sdf_effect_kind kind)
    {
        switch (kind)
        {
        case macos_sdf_effect_kind::none:
            return "nil";
        case macos_sdf_effect_kind::fill:
            return "CASDFFillEffect";
        case macos_sdf_effect_kind::gradient:
            return "CASDFGradientEffect";
        case macos_sdf_effect_kind::gradient_contour:
            return "CASDFGradientContourEffect";
        case macos_sdf_effect_kind::shadow:
            return "CASDFShadowEffect";
        case macos_sdf_effect_kind::glass_highlight:
            return "CASDFGlassHighlightEffect";
        case macos_sdf_effect_kind::glass_displacement:
            return "CASDFGlassDisplacementEffect";
        case macos_sdf_effect_kind::key_fill_highlight:
            return "CASDFKeyFillHighlightEffect";
        case macos_sdf_effect_kind::output:
            return "CASDFOutputEffect";
        case macos_sdf_effect_kind::visualization:
            return "CASDFVisualizationEffect";
        case macos_sdf_effect_kind::unmodelled:
            break;
        }

        return "an unrecognised CASDFEffect";
    }

    const macos_sdf_diagnostic* macos_sdf_stats::diagnostic(const std::string_view name) const
    {
        for (const auto& entry : this->diagnostics)
        {
            if (entry.name == name)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    bool macos_sdf_coverage_raster::valid() const
    {
        return this->values != nullptr && this->width > 0 && this->height > 0 && this->stride >= this->width;
    }

    double macos_sdf_round_box_distance(const macos_layer_point sample, const macos_layer_rect& bounds, const double corner_radius)
    {
        const auto rect = bounds.standardized();
        const auto half_x = rect.width / 2.0;
        const auto half_y = rect.height / 2.0;

        const auto point_x = sample.x - (rect.x + half_x);
        const auto point_y = sample.y - (rect.y + half_y);

        const auto q_x = std::abs(point_x) - half_x + corner_radius;
        const auto q_y = std::abs(point_y) - half_y + corner_radius;

        return std::min(std::max(q_x, q_y), 0.0) + std::hypot(std::max(q_x, 0.0), std::max(q_y, 0.0)) - corner_radius;
    }

    double macos_sdf_smooth_min(const double a, const double b, const double smoothness)
    {
        if (!finite(a) || !finite(b) || !finite(smoothness) || smoothness <= 0.0)
        {
            return std::min(a, b);
        }

        const auto h = std::max(smoothness - std::abs(a - b), 0.0) / smoothness;
        return std::min(a, b) - h * h * smoothness * 0.25;
    }

    double macos_sdf_smooth_max(const double a, const double b, const double smoothness)
    {
        return -macos_sdf_smooth_min(-a, -b, smoothness);
    }

    double macos_sdf_coverage(const double distance_device_pixels)
    {
        if (!finite(distance_device_pixels))
        {
            return distance_device_pixels < 0.0 ? 1.0 : 0.0;
        }

        return std::clamp(0.5 - distance_device_pixels, 0.0, 1.0);
    }

    macos_sdf_prepared_field macos_sdf_prepare(const macos_sdf_field& field, macos_sdf_stats& stats)
    {
        macos_sdf_prepared_field prepared{};
        prepared.smoothness = finite(field.smoothness) && field.smoothness > 0.0 ? field.smoothness : 0.0;
        prepared.effect_offset = finite(field.effect_offset) ? field.effect_offset : 0.0;

        if (finite(field.gaussian_radius) && field.gaussian_radius > 0.0)
        {
            note(stats, macos_sdf_diagnostic_kind::unmodelled, "gaussianRadius",
                 "the field is rendered unblurred; on the host a radius of 6 pulls the 50% contour in by "
                 "about a point on the horizontal edges and leaves the vertical ones alone (section 5.4)");
        }

        for (const auto& element : field.elements)
        {
            ++stats.elements_total;

            if (element.mode == macos_sdf_mode::contents)
            {
                ++stats.elements_refused;
                note(stats, macos_sdf_diagnostic_kind::unmodelled, "mode",
                     "\"contents\" takes the field from the element's contents image; the value-to-distance "
                     "map is known but the sampling geometry was never measured (section 3.2), and the host "
                     "renders nothing at all when the image is missing. The element contributes nothing");
                continue;
            }

            if (!finite_rect(element.bounds) || !finite(element.corner_radius))
            {
                ++stats.elements_refused;
                note(stats, macos_sdf_diagnostic_kind::unmodelled, "bounds",
                     "the element's bounds or cornerRadius is not finite; the element contributes nothing");
                continue;
            }

            const auto scale = similarity_scale(element.to_container);
            const auto inverse = element.to_container.inverse();

            if (!scale || !inverse)
            {
                ++stats.elements_refused;
                note(stats, macos_sdf_diagnostic_kind::unmodelled, "CASDFElementLayer.transform",
                     "the element's mapping into the container is not an invertible similarity, so its "
                     "distance has no single scale in container points; SwiftUI never sets an element "
                     "transform and the host's placement under one was never pinned down (section 2). The "
                     "element contributes nothing");
                continue;
            }

            macos_sdf_prepared_element ready{};
            ready.container_to_element = *inverse;
            ready.bounds = element.bounds.standardized();
            ready.corner_radius = element.corner_radius;
            ready.distance_scale = *scale;
            ready.operation = element.operation;

            prepared.elements.push_back(ready);
            ++stats.elements_used;

            if (element.operation != macos_sdf_operation::unite)
            {
                continue;
            }

            // A non-negative corner radius keeps the shape inside its bounds even when the radius is far
            // larger than the box: outside the box one of the sdRoundBox terms already exceeds the radius.
            const auto reach = inflated(ready.bounds, std::max(0.0, -ready.corner_radius));
            const auto box = mapped_bounding_box(element.to_container, reach);
            prepared.extent = prepared.has_extent ? united(prepared.extent, box) : box;
            prepared.has_extent = true;
        }

        return prepared;
    }

    macos_sdf_sample macos_sdf_sample_field(const macos_sdf_prepared_field& field, const macos_layer_point container_point)
    {
        constexpr auto outside = std::numeric_limits<double>::infinity();

        macos_sdf_sample sample{outside, outside};

        for (const auto& element : field.elements)
        {
            const auto local = element.container_to_element.apply(container_point);
            const auto distance = macos_sdf_round_box_distance(local, element.bounds, element.corner_radius) * element.distance_scale;

            if (element.operation == macos_sdf_operation::subtract)
            {
                sample.distance = macos_sdf_smooth_max(sample.distance, -distance, field.smoothness);
                sample.sharp_distance = std::max(sample.sharp_distance, -distance);
            }
            else
            {
                sample.distance = macos_sdf_smooth_min(sample.distance, distance, field.smoothness);
                sample.sharp_distance = std::min(sample.sharp_distance, distance);
            }
        }

        sample.distance += field.effect_offset;
        sample.sharp_distance += field.effect_offset;
        return sample;
    }

    double macos_sdf_distance(const macos_sdf_prepared_field& field, const macos_layer_point container_point)
    {
        return macos_sdf_sample_field(field, container_point).distance;
    }

    macos_sdf_stats macos_sdf_rasterize_coverage(const macos_sdf_field& field, const macos_layer_affine& container_to_device,
                                                 const macos_sdf_coverage_raster& raster)
    {
        macos_sdf_stats stats{};
        if (!raster.valid())
        {
            return stats;
        }

        for (int32_t y = 0; y < raster.height; ++y)
        {
            std::fill_n(raster.values + static_cast<size_t>(y) * static_cast<size_t>(raster.stride), raster.width, 0.0f);
        }

        const auto prepared = macos_sdf_prepare(field, stats);
        const auto plan = plan_raster(prepared, container_to_device, raster.width, raster.height, stats);
        if (!plan || plan->box.empty())
        {
            return stats;
        }

        for (auto y = plan->box.y0; y < plan->box.y1; ++y)
        {
            auto* row = raster.values + static_cast<size_t>(y) * static_cast<size_t>(raster.stride);

            for (auto x = plan->box.x0; x < plan->box.x1; ++x)
            {
                const macos_layer_point centre{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                const auto sample = macos_sdf_sample_field(prepared, plan->device_to_container.apply(centre));
                const auto coverage = macos_sdf_coverage(sample.distance * plan->scale);

                if (prepared.smoothness > 0.0 &&
                    std::abs(coverage - macos_sdf_coverage(sample.sharp_distance * plan->scale)) > COVERAGE_LEVEL)
                {
                    ++stats.smooth_blend_pixels;
                }

                row[x] = static_cast<float>(coverage);
                if (coverage > 0.0)
                {
                    ++stats.pixels_written;
                }
            }
        }

        note_smoothness_residual(stats, prepared);
        return stats;
    }

    macos_sdf_stats macos_sdf_render(const macos_sdf_field& field, const macos_sdf_effect& effect,
                                     const macos_layer_affine& container_to_device, const macos_layer_surface& surface)
    {
        macos_sdf_stats stats{};
        if (!surface.valid())
        {
            return stats;
        }

        if (effect.kind == macos_sdf_effect_kind::none)
        {
            return stats;
        }

        if (effect.kind == macos_sdf_effect_kind::visualization)
        {
            note(stats, macos_sdf_diagnostic_kind::unmodelled, "effect",
                 "CASDFVisualizationEffect writes a debug field that is not clipped to the layer's bounds "
                 "and floods the surface, and a nil effect renders byte-identically to it (section 6); "
                 "nothing was drawn");
            return stats;
        }

        if (effect.kind != macos_sdf_effect_kind::fill)
        {
            const auto named = effect.class_name.empty() ? std::string{macos_sdf_effect_kind_name(effect.kind)} : effect.class_name;
            note(stats, macos_sdf_diagnostic_kind::unmodelled, "effect",
                 named + " is not modelled; only CASDFFillEffect is measured well enough to reproduce "
                         "(section 6). Nothing was drawn");
            return stats;
        }

        if (!effect.color.present)
        {
            note(stats, macos_sdf_diagnostic_kind::unmodelled, "CASDFFillEffect.color",
                 "the fill colour was not readable; painting the property's own default of opaque white "
                 "would flood the surface, so nothing was drawn");
            return stats;
        }

        const auto prepared = macos_sdf_prepare(field, stats);
        const auto plan = plan_raster(prepared, container_to_device, surface.width, surface.height, stats);
        if (!plan || plan->box.empty())
        {
            return stats;
        }

        const auto color_alpha = clamp01(effect.color.a);
        const auto color_r = clamp01(effect.color.r);
        const auto color_g = clamp01(effect.color.g);
        const auto color_b = clamp01(effect.color.b);

        for (auto y = plan->box.y0; y < plan->box.y1; ++y)
        {
            auto* row = surface.pixels + static_cast<size_t>(y) * static_cast<size_t>(surface.stride);

            for (auto x = plan->box.x0; x < plan->box.x1; ++x)
            {
                const macos_layer_point centre{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
                const auto sample = macos_sdf_sample_field(prepared, plan->device_to_container.apply(centre));
                const auto coverage = macos_sdf_coverage(sample.distance * plan->scale);

                if (prepared.smoothness > 0.0 &&
                    std::abs(coverage - macos_sdf_coverage(sample.sharp_distance * plan->scale)) > COVERAGE_LEVEL)
                {
                    ++stats.smooth_blend_pixels;
                }

                const auto alpha = color_alpha * coverage;
                if (alpha <= 0.0)
                {
                    continue;
                }

                macos_layer_blend_pixel(row + static_cast<size_t>(x) * 4, macos_layer_channel_to_byte(color_b * alpha),
                                        macos_layer_channel_to_byte(color_g * alpha), macos_layer_channel_to_byte(color_r * alpha),
                                        macos_layer_channel_to_byte(alpha));
                ++stats.pixels_written;
            }
        }

        note_smoothness_residual(stats, prepared);
        return stats;
    }

    macos_layer_color macos_sdf_measured_glass_fill(const bool dark_appearance)
    {
        if (dark_appearance)
        {
            return {true, 30.0 / 255.0, 30.0 / 255.0, 30.0 / 255.0, 0.75};
        }

        return {true, 243.0 / 255.0, 243.0 / 255.0, 243.0 / 255.0, 0.90};
    }
}
