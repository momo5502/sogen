#pragma once

#include "macos_layer_compositor.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    // A standalone rasteriser for CASDFLayer / CASDFElementLayer. Every number in it comes from
 //, measured against a
    // CGWindowListCreateImage oracle on macOS 25G76; the section numbers below are that document's.
    // Nothing here reaches the emulator, the layer tree or guest memory: the caller collects the
    // elements out of the tree and hands over plain geometry in the container's own point space.

    // Section 1: `mode` and `operation` are NSStrings, not integers. QuartzCore's constant pool holds
    // exactly these spellings and the element's debug printer enumerates the alternatives exhaustively.
    enum class macos_sdf_mode : uint8_t
    {
        bounds,
        contents,
    };

    enum class macos_sdf_operation : uint8_t
    {
        unite,
        subtract,
    };

    std::optional<macos_sdf_mode> macos_sdf_mode_from_name(std::string_view name);
    std::optional<macos_sdf_operation> macos_sdf_operation_from_name(std::string_view name);

    enum class macos_sdf_effect_kind : uint8_t
    {
        // Section 6: a nil effect renders byte-identically to CASDFVisualizationEffect, an unclipped
        // debug field that floods the surface. Reproducing that would be actively harmful, so both this
        // and `visualization` mean draw nothing.
        none,
        fill,
        gradient,
        gradient_contour,
        shadow,
        glass_highlight,
        glass_displacement,
        key_fill_highlight,
        output,
        visualization,
        unmodelled,
    };

    std::optional<macos_sdf_effect_kind> macos_sdf_effect_kind_from_class_name(std::string_view class_name);
    std::string_view macos_sdf_effect_kind_name(macos_sdf_effect_kind kind);

    struct macos_sdf_effect
    {
        macos_sdf_effect_kind kind{macos_sdf_effect_kind::none};

        // The guest's own class name, used only to name a refusal.
        std::string class_name{};

        // CASDFFillEffect.color, straight (not premultiplied) sRGB.
        macos_layer_color color{};
    };

    // Section 2: the element's paint attributes are ignored, its geometry is not. `to_container` is the
    // ordinary CoreAnimation mapping from the element's own space into the container's, which the caller
    // already computes; a hidden element must not be handed over at all.
    struct macos_sdf_element
    {
        macos_layer_rect bounds{};
        double corner_radius{};
        macos_layer_affine to_container{};

        macos_sdf_mode mode{macos_sdf_mode::bounds};
        macos_sdf_operation operation{macos_sdf_operation::unite};

        // Section 3.2: inert while `mode` is "bounds", measured byte-identical at -5/+5.
        double contents_zero_value_distance{0.0};
        double contents_one_value_distance{1.0};

        // Section 3.3: only reparameterises a CASDFGradientEffect, which this rasteriser refuses anyway.
        double gradient_ovalization{0.0};
    };

    // Section 5.3: `merge_elements` chooses whether the effect runs once per element or once on the
    // combined field. With a flat fill it is measured byte-identical either way, so the only paint this
    // rasteriser implements makes it a no-op; how the merged field is built was never characterised.
    struct macos_sdf_field
    {
        std::vector<macos_sdf_element> elements{};
        double smoothness{};
        double gaussian_radius{};
        double effect_offset{};
        bool merge_elements{};
    };

    enum class macos_sdf_diagnostic_kind : uint8_t
    {
        unmodelled,
        approximated,
    };

    struct macos_sdf_diagnostic
    {
        macos_sdf_diagnostic_kind kind{};
        std::string name{};
        std::string detail{};
    };

    struct macos_sdf_stats
    {
        size_t elements_total{};
        size_t elements_used{};
        size_t elements_refused{};
        size_t pixels_written{};

        // Pixels where `smoothness` moved the coverage by more than one 8-bit level.
        size_t smooth_blend_pixels{};
        std::vector<macos_sdf_diagnostic> diagnostics{};

        const macos_sdf_diagnostic* diagnostic(std::string_view name) const;
    };

    struct macos_sdf_prepared_element
    {
        macos_layer_affine container_to_element{};
        macos_layer_rect bounds{};
        double corner_radius{};
        double distance_scale{1.0};
        macos_sdf_operation operation{macos_sdf_operation::unite};
    };

    struct macos_sdf_prepared_field
    {
        std::vector<macos_sdf_prepared_element> elements{};
        double smoothness{};
        double effect_offset{};

        // Container-space bounding box of the uniting elements. The field is deliberately not clipped to
        // the container's bounds and ignores its masksToBounds (section 2), so this is the only extent
        // the rasteriser has.
        macos_layer_rect extent{};
        bool has_extent{};
    };

    macos_sdf_prepared_field macos_sdf_prepare(const macos_sdf_field& field, macos_sdf_stats& stats);

    struct macos_sdf_sample
    {
        double distance{};

        // The same fold with a plain min/max, so a caller can see whether `smoothness` changed anything.
        double sharp_distance{};
    };

    // Both distances are in container points, after `effect_offset`, and positive outside.
    macos_sdf_sample macos_sdf_sample_field(const macos_sdf_prepared_field& field, macos_layer_point container_point);
    double macos_sdf_distance(const macos_sdf_prepared_field& field, macos_layer_point container_point);

    // Section 3.1: Inigo Quilez's sdRoundBox with the corner radius NOT clamped to half the shorter
    // side. The clamp is what a reader will want to add, and it is wrong: an 80x50 element with
    // cornerRadius 100 renders a 12.3 x 10.0 rounded diamond on the host, which only the unclamped form
    // produces.
    double macos_sdf_round_box_distance(macos_layer_point sample, const macos_layer_rect& bounds, double corner_radius);

    // Section 5.2: exact at smoothness 2 and at every measured gap configuration, including k=8 and
    // k=16, but it over-predicts the junction of two tangent circles by 8% at k=8 and 25% at k=32. The
    // real operator is unknown. Rendering reports the residual by name when the blend actually engages.
    constexpr double MACOS_SDF_SMOOTHNESS_MEASURED_EXACT = 2.0;

    double macos_sdf_smooth_min(double a, double b, double smoothness);
    double macos_sdf_smooth_max(double a, double b, double smoothness);

    // Section 4: clamp(0.5 - d, 0, 1) with d in device pixels. A one-device-pixel ramp centred on the
    // surface, which for a straight edge is exact area coverage; there is no second antialiasing pass.
    double macos_sdf_coverage(double distance_device_pixels);

    struct macos_sdf_coverage_raster
    {
        float* values{};
        int32_t width{};
        int32_t height{};

        // Floats per row, not bytes.
        int32_t stride{};

        bool valid() const;
    };

    // `container_to_device` maps container points to device pixels; device pixel (x, y) is sampled at its
    // centre (x + 0.5, y + 0.5). Both entry points refuse a mapping that is not a similarity, because the
    // coverage ramp is one device pixel wide and a sheared mapping gives it no single width.

    // Writes every pixel of the raster, 0 where the field does not reach.
    macos_sdf_stats macos_sdf_rasterize_coverage(const macos_sdf_field& field, const macos_layer_affine& container_to_device,
                                                 const macos_sdf_coverage_raster& raster);

    // Source-over into a BGRA8 premultiplied top-down surface, the same arithmetic macos_layer_composite
    // uses, so a field rendered here and a layer rendered there compose identically.
    macos_sdf_stats macos_sdf_render(const macos_sdf_field& field, const macos_sdf_effect& effect,
                                     const macos_layer_affine& container_to_device, const macos_layer_surface& surface);

    // Section 7: a Liquid Glass bezel composites as a flat translucent fill to within 5/255 over a 4:1
    // backdrop range. These are the midpoints of the six measured fits, offered so a caller applying the
    // section 9 shortcut does not re-derive them. It is an approximation, not a model of the glass.
    macos_layer_color macos_sdf_measured_glass_fill(bool dark_appearance);
}
