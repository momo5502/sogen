#pragma once

#include "macos_layer_tree.hpp"

#include <cstddef>
#include <cstdint>

namespace sogen
{
    // BGRA8 premultiplied, rows top-down -- the format ui_surface_format::bgra8 and the window backing
    // store already use, so a composited frame is presented without a conversion pass.
    struct macos_layer_surface
    {
        uint8_t* pixels{};
        int32_t width{};
        int32_t height{};
        int32_t stride{};

        bool valid() const;
    };

    // Layer contents live in guest memory, which this file must not know how to reach: the compositor is
    // built for the wasm target as well, with no emulator and no platform headers behind it.
    using macos_layer_pixel_reader = bool (*)(void* context, uint64_t address, void* destination, size_t size);

    struct macos_layer_pixel_source
    {
        macos_layer_pixel_reader read{};
        void* context{};
    };

    struct macos_layer_composite_options
    {
        // Points to device pixels. A Retina window backing store is twice the layer geometry.
        double scale{1.0};

        // The root layer's space is y-up (Cocoa); a backing store's rows run top-down. Measured on
        // 25G76 by rendering a layer tree through -[CALayer renderInContext:] and reading the bytes back:
        // a child at layer y in [1,3] of an 8-high root lands in buffer rows 5 and 6.
        bool flip_y{true};

        double origin_x{};
        double origin_y{};

        // Cleared to transparent black before the tree is drawn. A window that is opaque wants the
        // backing store left alone instead, because its own root layer may not cover every pixel.
        bool clear{true};
    };

    struct macos_layer_composite_stats
    {
        size_t layers_visited{};
        size_t layers_drawn{};
        size_t pixels_written{};
        size_t contents_blits{};
        size_t contents_unresolved{};
        size_t contents_unreadable{};
        size_t depth_limited{};
        size_t cycles_cut{};
        size_t layer_limited{};
        size_t clip_limited{};
        size_t singular_transforms{};
        size_t masks_applied{};
        size_t masks_skipped{};
        size_t portals_unmodelled{};
        size_t sdf_fields_drawn{};
        size_t sdf_fields_approximated{};
        size_t sdf_fields_unmodelled{};
        size_t sdf_elements_used{};
        size_t sdf_elements_refused{};
        size_t shapes_filled{};
        size_t shapes_unmodelled{};
    };

    constexpr size_t MACOS_LAYER_MAX_DEPTH = 64;
    constexpr size_t MACOS_LAYER_MAX_NODES_PER_COMPOSITE = 4096;
    constexpr size_t MACOS_LAYER_MAX_CLIPS = 64;
    constexpr size_t MACOS_LAYER_MAX_MASKS = 16;
    constexpr size_t MACOS_LAYER_MAX_MASK_SHAPES = 256;

    macos_layer_composite_stats macos_layer_composite(const macos_layer_tree& tree, uint64_t root, const macos_layer_surface& surface,
                                                      const macos_layer_composite_options& options, const macos_layer_pixel_source& source);

    // The mapping the compositor uses for a layer's own coordinate space, exposed so the geometry can be
    // checked against CoreAnimation's -[CALayer convertPoint:toLayer:] without rasterising anything.
    // Absent when the layer is not in the tree, when an ancestor link is broken, or when the chain is
    // deeper than MACOS_LAYER_MAX_DEPTH.
    std::optional<macos_layer_affine> macos_layer_transform_to_root(const macos_layer_tree& tree, uint64_t root, uint64_t layer);

    // Source-over with premultiplied 8-bit components, matching CoreAnimation's own arithmetic: opaque
    // red at opacity 0.5 over opaque white measured as (0xff, 0x7f, 0x7f).
    void macos_layer_blend_pixel(uint8_t* destination, uint8_t b, uint8_t g, uint8_t r, uint8_t a);

    uint8_t macos_layer_channel_to_byte(double value);
}
