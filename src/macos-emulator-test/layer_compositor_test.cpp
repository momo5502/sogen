#include <gtest/gtest.h>

#include <gui/macos_layer_compositor.hpp>
#include <gui/macos_sdf_field.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The expected numbers in this file are not re-derivations of the compositor's own arithmetic: the
// geometry cases are what CoreAnimation's -[CALayer convertPoint:toLayer:] answered on macOS 25G76
// (src/tools/macos-gui-probe/layergeom.m) and the raster cases are the bytes -[CALayer renderInContext:]
// wrote into a CGBitmapContext for the same tree. Provenance and the full measurement log:

namespace
{
    using sogen::macos_layer_affine;
    using sogen::macos_layer_color;
    using sogen::macos_layer_gravity;
    using sogen::macos_layer_node;
    using sogen::macos_layer_point;
    using sogen::macos_layer_raster;
    using sogen::macos_layer_rect;
    using sogen::macos_layer_surface;
    using sogen::macos_layer_tree;

    constexpr uint64_t RASTER_BASE = 0x4000;

    macos_layer_node& make(macos_layer_tree& tree, const uint64_t id, const macos_layer_rect bounds, const macos_layer_point position,
                           const macos_layer_point anchor)
    {
        auto& node = tree.touch(id);
        node.bounds = bounds.standardized();
        node.position = position;
        node.anchor_point = anchor;
        return node;
    }

    macos_layer_color rgba(const double r, const double g, const double b, const double a)
    {
        return {true, r, g, b, a};
    }

    struct raster_memory
    {
        std::vector<uint8_t> bytes{};

        static bool read(void* context, const uint64_t address, void* destination, const size_t size)
        {
            const auto& self = *static_cast<raster_memory*>(context);
            if (address < RASTER_BASE)
            {
                return false;
            }

            const auto offset = address - RASTER_BASE;
            if (offset + size > self.bytes.size())
            {
                return false;
            }

            std::memcpy(destination, self.bytes.data() + offset, size);
            return true;
        }
    };

    // Row 0 = (red, green), row 1 = (blue, white), BGRA premultiplied, top-down -- the same image
    // render.m handed to CoreAnimation.
    raster_memory two_by_two()
    {
        return {{
            0x00,
            0x00,
            0xFF,
            0xFF,
            0x00,
            0xFF,
            0x00,
            0xFF, //
            0xFF,
            0x00,
            0x00,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF, //
        }};
    }

    macos_layer_raster two_by_two_raster()
    {
        return {RASTER_BASE, 2, 2, 8};
    }

    struct canvas
    {
        int32_t width{};
        int32_t height{};
        std::vector<uint8_t> pixels{};

        explicit canvas(const int32_t w, const int32_t h)
            : width(w),
              height(h),
              pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0)
        {
        }

        macos_layer_surface surface()
        {
            return {this->pixels.data(), this->width, this->height, this->width * 4};
        }

        uint32_t argb(const int32_t x, const int32_t y) const
        {
            const auto* p = this->pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(this->width) + static_cast<size_t>(x)) * 4u;
            return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
        }

        std::string row(const int32_t y) const
        {
            std::string text{};
            for (int32_t x = 0; x < this->width; ++x)
            {
                char buffer[16]{};
                std::snprintf(buffer, sizeof(buffer), "%08x ", this->argb(x, y));
                text += buffer;
            }

            return text;
        }
    };

    sogen::macos_layer_composite_stats render(const macos_layer_tree& tree, const uint64_t root, canvas& target, raster_memory& memory)
    {
        const sogen::macos_layer_pixel_source source{raster_memory::read, &memory};
        return macos_layer_composite(tree, root, target.surface(), {}, source);
    }

    struct corner_expectation
    {
        const char* name{};
        macos_layer_point corners[4]{};
    };

    void expect_corners(const macos_layer_tree& tree, const uint64_t root, const uint64_t layer, const corner_expectation& expected)
    {
        const auto mapping = sogen::macos_layer_transform_to_root(tree, root, layer);
        ASSERT_TRUE(mapping.has_value()) << expected.name;

        const auto* node = tree.find(layer);
        ASSERT_NE(node, nullptr) << expected.name;

        const auto bounds = node->bounds.standardized();
        const macos_layer_point locals[4] = {
            {bounds.x, bounds.y},
            {bounds.x + bounds.width, bounds.y},
            {bounds.x, bounds.y + bounds.height},
            {bounds.x + bounds.width, bounds.y + bounds.height},
        };

        for (size_t i = 0; i < 4; ++i)
        {
            const auto mapped = mapping->apply(locals[i]);
            EXPECT_NEAR(mapped.x, expected.corners[i].x, 1e-4) << expected.name << " corner " << i << " x";
            EXPECT_NEAR(mapped.y, expected.corners[i].y, 1e-4) << expected.name << " corner " << i << " y";
        }
    }

    macos_layer_tree with_root(uint64_t& root)
    {
        macos_layer_tree tree{};
        root = 1;
        make(tree, root, {0, 0, 400, 300}, {200, 150}, {0.5, 0.5});
        return tree;
    }

    TEST(LayerCompositor, ReproducesCoreAnimationCornerMappingForAnchorsAndTransforms)
    {
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"anchor00", {{30, 20}, {130, 20}, {30, 70}, {130, 70}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0.5, 0.5});
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"anchor_center", {{-20, -5}, {80, -5}, {-20, 45}, {80, 45}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {-11.5, -4.5, 111, 26.5}, {44, 8.75}, {0.5, 0.5});
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"bounds_origin", {{-11.5, -4.5}, {99.5, -4.5}, {-11.5, 22}, {99.5, 22}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0}).transform = macos_layer_affine::scaling(2, 3);
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"scale_anchor00", {{30, 20}, {230, 20}, {30, 170}, {230, 170}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0.5, 0.5}).transform = macos_layer_affine::scaling(2, 3);
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"scale_center", {{-70, -55}, {130, -55}, {-70, 95}, {130, 95}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            const auto angle = std::acos(-1.0) / 6.0;
            make(tree, 2, {0, 0, 100, 50}, {200, 100}, {0.5, 0.5}).transform = {
                std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0.0, 0.0};
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2,
                           {"rotate30_center", {{169.1987, 53.3494}, {255.8013, 103.3494}, {144.1987, 96.6506}, {230.8013, 146.6506}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0}).transform = {1.5, 0.25, -0.5, 2.0, 7.0, -3.0};
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"full_affine", {{37, 17}, {187, 42}, {12, 117}, {162, 142}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0}).transform = macos_layer_affine{1, 0, 0, 1, 11, 13};
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"transform3d_translate", {{41, 33}, {141, 33}, {41, 83}, {141, 83}}});
        }
    }

    TEST(LayerCompositor, ReproducesCoreAnimationCornerMappingForGeometryFlipped)
    {
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            tree.touch(root).geometry_flipped = true;
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            // The destination layer's own flip is not part of a child -> root mapping.
            expect_corners(tree, root, 2, {"gflip_parent", {{30, 20}, {130, 20}, {30, 70}, {130, 70}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0}).geometry_flipped = true;
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"child_own_flip", {{30, 70}, {130, 70}, {30, 20}, {130, 20}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"mid_flip_to_root", {{80, 170}, {180, 170}, {80, 120}, {180, 120}}});
            expect_corners(tree, 2, 3, {"mid_flip_to_mid", {{30, 20}, {130, 20}, {30, 70}, {130, 70}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 7, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            // The mirror is about the bounds' vertical centre, not about its height: 2*7 + 150 - y.
            expect_corners(tree, root, 3, {"mid_flip_boundsorigin", {{80, 177}, {180, 177}, {80, 127}, {180, 127}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0.5, 0.5});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"mid_flip_anchor_center", {{30, 195}, {130, 195}, {30, 145}, {130, 145}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0}).transform = {1.5, 0.25, -0.5, 2.0, 7.0, -3.0};
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"mid_flip_child_affine", {{87, 173}, {237, 148}, {62, 73}, {212, 48}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            mid.transform = macos_layer_affine::scaling(1, 2);
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"nested_gflip_scale", {{80, 300}, {180, 300}, {80, 200}, {180, 200}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0.5, 0.5});
            mid.geometry_flipped = true;
            mid.transform = {1.5, 0.25, -0.5, 2.0, 7.0, -3.0};
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"flip_with_affine", {{-75.5, 129.5}, {74.5, 154.5}, {-50.5, 29.5}, {99.5, 54.5}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            tree.touch(root).geometry_flipped = true;
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"double_gflip", {{80, 170}, {180, 170}, {80, 120}, {180, 120}}});
        }

        const double anchors[3] = {0.0, 0.5, 1.0};
        const macos_layer_point expected[3][4] = {
            {{27, 177}, {127, 177}, {27, 127}, {127, 127}},
            {{27, 102}, {127, 102}, {27, 52}, {127, 52}},
            {{27, 27}, {127, 27}, {27, -23}, {127, -23}},
        };

        for (size_t i = 0; i < 3; ++i)
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {3, 7, 200, 150}, {50, 40}, {0.25, anchors[i]});
            mid.geometry_flipped = true;
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);

            corner_expectation expectation{"flip_anchor_y", {}};
            for (size_t c = 0; c < 4; ++c)
            {
                expectation.corners[c] = expected[i][c];
            }

            expect_corners(tree, root, 3, expectation);
        }
    }

    TEST(LayerCompositor, ReproducesCoreAnimationCornerMappingForSublayerTransforms)
    {
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.sublayer_transform = macos_layer_affine::scaling(2, 0.5);
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"sublayer_transform", {{110, 50}, {310, 50}, {110, 75}, {310, 75}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {5, 9, 200, 150}, {50, 40}, {0.5, 0.5});
            mid.sublayer_transform = macos_layer_affine::scaling(2, 0.5);
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"subxform_anchor_center", {{-100, 8}, {100, 8}, {-100, 33}, {100, 33}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            auto& mid = make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            mid.geometry_flipped = true;
            mid.sublayer_transform = macos_layer_affine::scaling(2, 0.5);
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            // The sublayer transform runs before the parent's flip.
            expect_corners(tree, root, 3, {"subxform_plus_flip", {{110, 180}, {310, 180}, {110, 155}, {310, 155}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            tree.touch(root).sublayer_transform = macos_layer_affine::scaling(3, 4);
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"root_subxform", {{-310, -370}, {-10, -370}, {-310, -170}, {-10, -170}}});
        }
        {
            uint64_t root = 0;
            macos_layer_tree tree{};
            make(tree, 1, {10, 20, 400, 300}, {200, 150}, {0.5, 0.5});
            root = 1;
            make(tree, 2, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            // A parent's bounds origin does not offset its children.
            expect_corners(tree, root, 2, {"root_bounds_origin", {{30, 20}, {130, 20}, {30, 70}, {130, 70}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 200, 150}, {50, 40}, {0, 0});
            make(tree, 3, {0, 0, 100, 50}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            tree.add_sublayer(2, 3);
            expect_corners(tree, root, 3, {"nested", {{80, 60}, {180, 60}, {80, 110}, {180, 110}}});
        }
    }

    TEST(LayerCompositor, ReproducesCoreAnimationCornerMappingForDegenerateBounds)
    {
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, 0, 0}, {30, 20}, {0.5, 0.5});
            tree.add_sublayer(root, 2);
            expect_corners(tree, root, 2, {"zero_size", {{30, 20}, {30, 20}, {30, 20}, {30, 20}}});
        }
        {
            uint64_t root = 0;
            auto tree = with_root(root);
            make(tree, 2, {0, 0, -40, -20}, {30, 20}, {0, 0});
            tree.add_sublayer(root, 2);
            EXPECT_DOUBLE_EQ(tree.find(2)->bounds.x, -40.0);
            EXPECT_DOUBLE_EQ(tree.find(2)->bounds.y, -20.0);
            expect_corners(tree, root, 2, {"negative_size", {{30, 20}, {70, 20}, {30, 40}, {70, 40}}});
        }
    }

    TEST(LayerCompositor, TransformToRootRefusesBrokenChains)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 10, 10}, {0, 0}, {0, 0});
        make(tree, 2, {0, 0, 10, 10}, {0, 0}, {0, 0});

        EXPECT_FALSE(sogen::macos_layer_transform_to_root(tree, 1, 2).has_value()) << "layer 2 is not attached to layer 1";
        EXPECT_FALSE(sogen::macos_layer_transform_to_root(tree, 1, 99).has_value());
        EXPECT_FALSE(sogen::macos_layer_transform_to_root(tree, 99, 1).has_value());
        EXPECT_TRUE(sogen::macos_layer_transform_to_root(tree, 1, 1).has_value());

        uint64_t previous = 1;
        for (uint64_t id = 100; id < 100 + sogen::MACOS_LAYER_MAX_DEPTH + 4; ++id)
        {
            make(tree, id, {0, 0, 10, 10}, {0, 0}, {0, 0});
            tree.add_sublayer(previous, id);
            previous = id;
        }

        EXPECT_FALSE(sogen::macos_layer_transform_to_root(tree, 1, previous).has_value()) << "the chain is deeper than the limit";
    }

    TEST(LayerCompositor, MatchesCoreAnimationRasterForBackgroundGeometry)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 8, 8}, {4, 4}, {0.5, 0.5}).background = rgba(1, 1, 1, 1);
        make(tree, 2, {0, 0, 4, 2}, {2, 1}, {0, 0}).background = rgba(1, 0, 0, 1);
        tree.add_sublayer(1, 2);

        canvas target{8, 8};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        for (int32_t y = 0; y < 8; ++y)
        {
            for (int32_t x = 0; x < 8; ++x)
            {
                const bool red = (y == 5 || y == 6) && x >= 2 && x <= 5;
                EXPECT_EQ(target.argb(x, y), red ? 0xFFFF0000u : 0xFFFFFFFFu) << "pixel " << x << "," << y << " row " << target.row(y);
            }
        }
    }

    TEST(LayerCompositor, MatchesCoreAnimationRasterForContentsGravity)
    {
        const struct
        {
            macos_layer_gravity gravity;
            uint32_t expected[4][4];
        } cases[] = {
            {macos_layer_gravity::resize,
             {{0xFFFF0000, 0xFFFF0000, 0xFF00FF00, 0xFF00FF00},
              {0xFFFF0000, 0xFFFF0000, 0xFF00FF00, 0xFF00FF00},
              {0xFF0000FF, 0xFF0000FF, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFF0000FF, 0xFF0000FF, 0xFFFFFFFF, 0xFFFFFFFF}}},
            {macos_layer_gravity::top_left,
             {{0xFFFF0000, 0xFF00FF00, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFF0000FF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}}},
            {macos_layer_gravity::bottom_right,
             {{0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
              {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00},
              {0xFFFFFFFF, 0xFFFFFFFF, 0xFF0000FF, 0xFFFFFFFF}}},
        };

        for (const auto& entry : cases)
        {
            macos_layer_tree tree{};
            auto& root = make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
            root.background = rgba(1, 1, 1, 1);
            root.gravity = entry.gravity;
            tree.attach_contents_raster(1, two_by_two_raster());

            canvas target{4, 4};
            auto memory = two_by_two();
            const auto stats = render(tree, 1, target, memory);
            EXPECT_EQ(stats.contents_blits, 1u);

            for (int32_t y = 0; y < 4; ++y)
            {
                for (int32_t x = 0; x < 4; ++x)
                {
                    EXPECT_EQ(target.argb(x, y), entry.expected[y][x])
                        << "gravity " << static_cast<int>(entry.gravity) << " pixel " << x << "," << y << " row " << target.row(y);
                }
            }
        }
    }

    // Measured on 25G76: geometryFlipped mirrors where a layer's sublayers land, and nothing else. The
    // same layer rendered through its superlayer produces byte-identical contents with the flag on and
    // off; only -[CALayer renderInContext:] called *on* the flipped layer mirrors, and that is the
    // entry point flipping its own context rather than the compositing rule. sogen always composites
    // from a window's root layer, so the sublayer answer is the one that applies.
    TEST(LayerCompositor, GeometryFlippedMovesSublayersWithoutTurningTheirContentsOver)
    {
        const uint32_t upright[4][4] = {
            {0xFFFF0000, 0xFFFF0000, 0xFF00FF00, 0xFF00FF00},
            {0xFFFF0000, 0xFFFF0000, 0xFF00FF00, 0xFF00FF00},
            {0xFF0000FF, 0xFF0000FF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFF0000FF, 0xFF0000FF, 0xFFFFFFFF, 0xFFFFFFFF},
        };

        for (const auto flipped : {false, true})
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
            auto& child = make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
            child.geometry_flipped = flipped;
            tree.attach_contents_raster(2, two_by_two_raster());
            tree.add_sublayer(1, 2);

            canvas target{4, 4};
            auto memory = two_by_two();
            render(tree, 1, target, memory);

            for (int32_t y = 0; y < 4; ++y)
            {
                for (int32_t x = 0; x < 4; ++x)
                {
                    EXPECT_EQ(target.argb(x, y), upright[y][x])
                        << "geometryFlipped=" << flipped << " pixel " << x << "," << y << " row " << target.row(y);
                }
            }
        }
    }

    TEST(LayerCompositor, GeometryFlippedRelocatesASublayerWithoutMirroringIt)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 4, 4}, {0, 0}, {0, 0});
        auto& host = make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
        host.geometry_flipped = true;
        tree.add_sublayer(1, 2);

        auto& marker = make(tree, 3, {0, 0, 4, 1}, {0, 0}, {0, 0});
        marker.background = rgba(1, 0, 0, 1);
        tree.add_sublayer(2, 3);

        canvas target{4, 4};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        EXPECT_EQ(target.argb(0, 0), 0xFFFF0000u) << "a child at the bottom of a flipped host draws at the top";
        EXPECT_EQ(target.argb(0, 3), 0x00000000u);
    }

    TEST(LayerCompositor, MatchesCoreAnimationBlendingForOpacityAndPremultipliedAlpha)
    {
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5}).background = rgba(1, 1, 1, 1);
            auto& child = make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
            child.background = rgba(1, 0, 0, 1);
            child.opacity = 0.5;
            tree.add_sublayer(1, 2);

            canvas target{4, 4};
            auto memory = two_by_two();
            render(tree, 1, target, memory);

            for (int32_t y = 0; y < 4; ++y)
            {
                for (int32_t x = 0; x < 4; ++x)
                {
                    EXPECT_EQ(target.argb(x, y), 0xFFFF7F7Fu) << "pixel " << x << "," << y;
                }
            }
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5}).background = rgba(0, 0, 1, 0.5);

            canvas target{4, 4};
            auto memory = two_by_two();
            render(tree, 1, target, memory);

            for (int32_t y = 0; y < 4; ++y)
            {
                for (int32_t x = 0; x < 4; ++x)
                {
                    EXPECT_EQ(target.argb(x, y), 0x80000080u) << "pixel " << x << "," << y;
                }
            }
        }
    }

    TEST(LayerCompositor, InheritsOpacityDownTheSubtree)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5}).background = rgba(1, 1, 1, 1);
        auto& mid = make(tree, 2, {0, 0, 2, 2}, {0, 0}, {0, 0});
        mid.opacity = 0.5;
        auto& leaf = make(tree, 3, {0, 0, 2, 2}, {0, 0}, {0, 0});
        leaf.background = rgba(0, 0, 0, 1);
        leaf.opacity = 0.5;
        tree.add_sublayer(1, 2);
        tree.add_sublayer(2, 3);

        canvas target{2, 2};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        // 0.5 * 0.5 = 0.25 -> alpha 64; black over white leaves 255 - 64 = 191 in every colour channel.
        EXPECT_EQ(target.argb(0, 0), 0xFFBFBFBFu);
    }

    TEST(LayerCompositor, MatchesCoreAnimationRasterForMasksToBounds)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
        root.background = rgba(1, 1, 1, 1);
        root.masks_to_bounds = true;
        make(tree, 2, {0, 0, 8, 8}, {2, 2}, {0, 0}).background = rgba(0, 0, 1, 1);
        tree.add_sublayer(1, 2);

        canvas target{4, 4};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        for (int32_t y = 0; y < 4; ++y)
        {
            for (int32_t x = 0; x < 4; ++x)
            {
                const bool blue = y <= 1 && x >= 2;
                EXPECT_EQ(target.argb(x, y), blue ? 0xFF0000FFu : 0xFFFFFFFFu) << "pixel " << x << "," << y << " row " << target.row(y);
            }
        }
    }

    TEST(LayerCompositor, ClipsThroughARotatedAncestorRatherThanItsBoundingBox)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 16, 16}, {8, 8}, {0.5, 0.5});
        root.background = rgba(1, 1, 1, 1);

        auto& clip = make(tree, 2, {0, 0, 8, 8}, {8, 8}, {0.5, 0.5});
        clip.masks_to_bounds = true;
        const auto angle = std::acos(-1.0) / 4.0;
        clip.transform = {std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0.0, 0.0};

        make(tree, 3, {-64, -64, 128, 128}, {0, 0}, {0, 0}).background = rgba(0, 0, 0, 1);
        tree.add_sublayer(1, 2);
        tree.add_sublayer(2, 3);

        canvas target{16, 16};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        // The clip is a 45-degree diamond around the centre. Its bounding box would swallow the corners.
        EXPECT_EQ(target.argb(8, 8), 0xFF000000u) << "the centre is inside the rotated clip";
        EXPECT_EQ(target.argb(3, 3), 0xFFFFFFFFu) << "a corner of the clip's bounding box is outside the clip itself";
        EXPECT_EQ(target.argb(12, 12), 0xFFFFFFFFu);
    }

    TEST(LayerCompositor, RoundsCornersTheWayCoreAnimationDoesApartFromAntialiasing)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 6, 6}, {3, 3}, {0.5, 0.5});
        root.background = rgba(0, 0, 0, 1);
        root.corner_radius = 3.0;

        canvas target{6, 6};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        for (int32_t y = 0; y < 6; ++y)
        {
            for (int32_t x = 0; x < 6; ++x)
            {
                const bool corner = (x == 0 || x == 5) && (y == 0 || y == 5);
                EXPECT_EQ(target.argb(x, y), corner ? 0x00000000u : 0xFF000000u) << "pixel " << x << "," << y << " row " << target.row(y);
            }
        }
    }

    TEST(LayerCompositor, ClampsCornerRadiusToHalfTheShorterSide)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 6, 6}, {3, 3}, {0.5, 0.5});
        root.background = rgba(0, 0, 0, 1);
        root.corner_radius = 1000.0;

        canvas target{6, 6};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        EXPECT_EQ(target.argb(0, 0), 0x00000000u);
        EXPECT_EQ(target.argb(3, 3), 0xFF000000u);
    }

    TEST(LayerCompositor, MatchesCoreAnimationRasterForAnInsideBorder)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 6, 6}, {3, 3}, {0.5, 0.5});
        root.background = rgba(1, 1, 1, 1);
        root.border = rgba(1, 0, 0, 1);
        root.border_width = 1.0;

        canvas target{6, 6};
        auto memory = two_by_two();
        render(tree, 1, target, memory);

        for (int32_t y = 0; y < 6; ++y)
        {
            for (int32_t x = 0; x < 6; ++x)
            {
                const bool edge = x == 0 || x == 5 || y == 0 || y == 5;
                EXPECT_EQ(target.argb(x, y), edge ? 0xFFFF0000u : 0xFFFFFFFFu) << "pixel " << x << "," << y << " row " << target.row(y);
            }
        }
    }

    TEST(LayerCompositor, DrawsChildrenInSublayerOrder)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
        make(tree, 2, {0, 0, 2, 2}, {0, 0}, {0, 0}).background = rgba(1, 0, 0, 1);
        make(tree, 3, {0, 0, 2, 2}, {0, 0}, {0, 0}).background = rgba(0, 1, 0, 1);
        tree.add_sublayer(1, 2);
        tree.add_sublayer(1, 3);

        canvas target{2, 2};
        auto memory = two_by_two();
        render(tree, 1, target, memory);
        EXPECT_EQ(target.argb(0, 0), 0xFF00FF00u) << "the last sublayer wins";

        tree.insert_sublayer(1, 2, 1);
        canvas again{2, 2};
        render(tree, 1, again, memory);
        EXPECT_EQ(again.argb(0, 0), 0xFFFF0000u) << "moving a sublayer to the end changes who wins";
    }

    TEST(LayerCompositor, HonoursFlipAndScaleOptions)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
        make(tree, 2, {0, 0, 4, 1}, {0, 0}, {0, 0}).background = rgba(1, 0, 0, 1);
        tree.add_sublayer(1, 2);

        auto memory = two_by_two();
        const sogen::macos_layer_pixel_source source{raster_memory::read, &memory};

        canvas flipped{4, 4};
        macos_layer_composite(tree, 1, flipped.surface(), {}, source);
        EXPECT_EQ(flipped.argb(0, 3), 0xFFFF0000u) << "layer y in [0,1] is the bottom row";
        EXPECT_EQ(flipped.argb(0, 0), 0u);

        canvas upright{4, 4};
        macos_layer_composite(tree, 1, upright.surface(), {.flip_y = false}, source);
        EXPECT_EQ(upright.argb(0, 0), 0xFFFF0000u);
        EXPECT_EQ(upright.argb(0, 3), 0u);

        canvas scaled{8, 8};
        macos_layer_composite(tree, 1, scaled.surface(), {.scale = 2.0}, source);
        EXPECT_EQ(scaled.argb(0, 7), 0xFFFF0000u);
        EXPECT_EQ(scaled.argb(0, 6), 0xFFFF0000u) << "one point of height is two device rows at scale 2";
        EXPECT_EQ(scaled.argb(0, 5), 0u);
    }

    TEST(LayerCompositor, HonoursTheOriginOffset)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5}).background = rgba(1, 0, 0, 1);

        canvas target{4, 4};
        auto memory = two_by_two();
        const sogen::macos_layer_pixel_source source{raster_memory::read, &memory};
        macos_layer_composite(tree, 1, target.surface(), {.origin_x = 2.0, .origin_y = 0.0}, source);

        EXPECT_EQ(target.argb(0, 3), 0xFFFF0000u);
        EXPECT_EQ(target.argb(1, 3), 0xFFFF0000u);
        EXPECT_EQ(target.argb(2, 3), 0u) << "the layer was shifted two points left";
    }

    TEST(LayerCompositor, SkipsHiddenAndFullyTransparentSubtrees)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
        auto& hidden = make(tree, 2, {0, 0, 2, 2}, {0, 0}, {0, 0});
        hidden.hidden = true;
        make(tree, 3, {0, 0, 2, 2}, {0, 0}, {0, 0}).background = rgba(1, 0, 0, 1);
        tree.add_sublayer(1, 2);
        tree.add_sublayer(2, 3);

        canvas target{2, 2};
        auto memory = two_by_two();
        auto stats = render(tree, 1, target, memory);
        EXPECT_EQ(target.argb(0, 0), 0u);
        EXPECT_EQ(stats.layers_visited, 2u) << "the hidden layer is visited, its subtree is not";

        tree.find(2)->hidden = false;
        tree.find(2)->opacity = 0.0;
        canvas transparent{2, 2};
        stats = render(tree, 1, transparent, memory);
        EXPECT_EQ(transparent.argb(0, 0), 0u);

        tree.find(2)->opacity = 1.0;
        canvas visible{2, 2};
        render(tree, 1, visible, memory);
        EXPECT_EQ(visible.argb(0, 0), 0xFFFF0000u);
    }

    TEST(LayerCompositor, ClampsOpacityOutsideTheUnitRange)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
        root.background = rgba(1, 0, 0, 1);
        root.opacity = 5.0;

        canvas target{2, 2};
        auto memory = two_by_two();
        render(tree, 1, target, memory);
        EXPECT_EQ(target.argb(0, 0), 0xFFFF0000u) << "CALayer stores an unclamped opacity; the compositor clamps it";

        tree.find(1)->opacity = -2.0;
        canvas negative{2, 2};
        render(tree, 1, negative, memory);
        EXPECT_EQ(negative.argb(0, 0), 0u);
    }

    TEST(LayerCompositor, SurvivesNonFiniteAndAbsurdGeometry)
    {
        const double nan_value = std::nan("");
        const double values[] = {nan_value, std::numeric_limits<double>::infinity(), 1e300, -1e300};

        for (const auto value : values)
        {
            macos_layer_tree tree{};
            auto& root = make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
            root.background = rgba(1, 1, 1, 1);

            auto& child = make(tree, 2, {0, 0, value, 4}, {value, value}, {0, 0});
            child.background = rgba(1, 0, 0, 1);
            child.transform = {value, 0, 0, value, value, value};
            child.corner_radius = value;
            child.border_width = value;
            child.opacity = value;
            tree.add_sublayer(1, 2);

            canvas target{4, 4};
            auto memory = two_by_two();
            render(tree, 1, target, memory);

            for (int32_t y = 0; y < 4; ++y)
            {
                for (int32_t x = 0; x < 4; ++x)
                {
                    EXPECT_EQ(target.argb(x, y), 0xFFFFFFFFu) << "value " << value << " pixel " << x << "," << y;
                }
            }
        }
    }

    TEST(LayerCompositor, ReportsASingularTransformInsteadOfDrawingThroughIt)
    {
        macos_layer_tree tree{};
        auto& root = make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5});
        root.background = rgba(1, 0, 0, 1);
        root.transform = macos_layer_affine::scaling(0.0, 1.0);

        canvas target{4, 4};
        auto memory = two_by_two();
        const auto stats = render(tree, 1, target, memory);

        EXPECT_EQ(stats.singular_transforms, 1u);
        EXPECT_EQ(target.argb(0, 0), 0u);
    }

    TEST(LayerCompositor, CutsCyclesAndCapsDepth)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
        make(tree, 2, {0, 0, 2, 2}, {0, 0}, {0, 0});
        tree.add_sublayer(1, 2);
        tree.find(2)->children.push_back(1);

        canvas target{2, 2};
        auto memory = two_by_two();
        const auto stats = render(tree, 1, target, memory);
        EXPECT_EQ(stats.cycles_cut, 1u);

        macos_layer_tree deep{};
        make(deep, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
        uint64_t previous = 1;
        for (uint64_t id = 2; id < 2 + sogen::MACOS_LAYER_MAX_DEPTH + 4; ++id)
        {
            make(deep, id, {0, 0, 2, 2}, {0, 0}, {0, 0});
            deep.add_sublayer(previous, id);
            previous = id;
        }

        canvas deep_target{2, 2};
        const auto deep_stats = render(deep, 1, deep_target, memory);
        EXPECT_EQ(deep_stats.depth_limited, 1u);
        EXPECT_EQ(deep_stats.layers_visited, sogen::MACOS_LAYER_MAX_DEPTH);
    }

    TEST(LayerCompositor, CountsContentsItCannotResolveOrRead)
    {
        {
            macos_layer_tree tree{};
            auto& root = make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
            root.contents.kind = sogen::macos_layer_contents_kind::unresolved;
            root.contents.object = 0xDEAD;

            canvas target{2, 2};
            auto memory = two_by_two();
            const auto stats = render(tree, 1, target, memory);
            EXPECT_EQ(stats.contents_unresolved, 1u);
            EXPECT_EQ(stats.contents_blits, 0u);
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5});
            tree.attach_contents_raster(1, {RASTER_BASE + 0x10000, 2, 2, 8});

            canvas target{2, 2};
            auto memory = two_by_two();
            const auto stats = render(tree, 1, target, memory);
            EXPECT_GT(stats.contents_unreadable, 0u);
            EXPECT_EQ(target.argb(0, 0), 0u);
        }
    }

    TEST(LayerCompositor, RejectsImplausibleRasterDescriptions)
    {
        const sogen::macos_layer_raster rasters[] = {
            {0, 2, 2, 8},           {RASTER_BASE, 0, 2, 8}, {RASTER_BASE, 2, 0, 8},
            {RASTER_BASE, 2, 2, 0}, {RASTER_BASE, 2, 2, 4}, {RASTER_BASE, 1u << 20, 2, 1u << 22},
        };

        for (const auto& raster : rasters)
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 2, 2}, {1, 1}, {0.5, 0.5}).background = rgba(1, 1, 1, 1);
            tree.attach_contents_raster(1, raster);

            canvas target{2, 2};
            auto memory = two_by_two();
            const auto stats = render(tree, 1, target, memory);
            EXPECT_EQ(stats.contents_blits, 0u) << "raster " << raster.width << "x" << raster.height << " stride " << raster.stride;
            EXPECT_EQ(target.argb(0, 0), 0xFFFFFFFFu);
        }
    }

    TEST(LayerCompositor, LeavesTheSurfaceAloneWhenAskedNotToClear)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 1, 1}, {0.5, 0.5}, {0.5, 0.5});

        canvas target{2, 2};
        std::ranges::fill(target.pixels, uint8_t{0x40});

        auto memory = two_by_two();
        const sogen::macos_layer_pixel_source source{raster_memory::read, &memory};
        macos_layer_composite(tree, 1, target.surface(), {.clear = false}, source);
        EXPECT_EQ(target.argb(1, 1), 0x40404040u);

        macos_layer_composite(tree, 1, target.surface(), {.clear = true}, source);
        EXPECT_EQ(target.argb(1, 1), 0u);
    }

    TEST(LayerCompositor, RefusesAnInvalidSurfaceOrAMissingRoot)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 4, 4}, {2, 2}, {0.5, 0.5}).background = rgba(1, 0, 0, 1);

        auto memory = two_by_two();
        const sogen::macos_layer_pixel_source source{raster_memory::read, &memory};

        std::vector<uint8_t> pixels(64, 0);
        const macos_layer_surface bad[] = {
            {nullptr, 4, 4, 16},
            {pixels.data(), 0, 4, 16},
            {pixels.data(), 4, 0, 16},
            {pixels.data(), 4, 4, 8},
        };

        for (const auto& surface : bad)
        {
            EXPECT_FALSE(surface.valid());
            const auto stats = macos_layer_composite(tree, 1, surface, {}, source);
            EXPECT_EQ(stats.layers_visited, 0u);
        }

        canvas target{4, 4};
        const auto stats = macos_layer_composite(tree, 99, target.surface(), {}, source);
        EXPECT_EQ(stats.layers_visited, 0u);
        EXPECT_EQ(target.argb(0, 0), 0u);
    }

    TEST(LayerCompositor, BlendsPremultipliedSourceOverExactly)
    {
        uint8_t pixel[4] = {0, 0, 0, 0};

        sogen::macos_layer_blend_pixel(pixel, 10, 20, 30, 0);
        EXPECT_EQ(pixel[0], 0) << "a zero-alpha source writes nothing";

        sogen::macos_layer_blend_pixel(pixel, 1, 2, 3, 255);
        EXPECT_EQ(pixel[0], 1);
        EXPECT_EQ(pixel[1], 2);
        EXPECT_EQ(pixel[2], 3);
        EXPECT_EQ(pixel[3], 255);

        uint8_t white[4] = {255, 255, 255, 255};
        sogen::macos_layer_blend_pixel(white, 0, 0, 128, 128);
        EXPECT_EQ(white[2], 255) << "measured: opaque red at opacity 0.5 over white is (0xff, 0x7f, 0x7f)";
        EXPECT_EQ(white[1], 127);
        EXPECT_EQ(white[0], 127);
        EXPECT_EQ(white[3], 255);

        uint8_t empty[4] = {0, 0, 0, 0};
        sogen::macos_layer_blend_pixel(empty, 128, 0, 0, 128);
        EXPECT_EQ(empty[0], 128) << "measured: rgba(0,0,1,0.5) stores as 0x80000080";
        EXPECT_EQ(empty[3], 128);
    }

    // A CALayer contents raster whose bytes are not in the premultiplied order the interface documents
    // reaches the blend with a colour channel above its own alpha; measured under sogen on a real
    // AppKit window separator, an ARGB raster arrived as (b=0xff, a=0xd9) and wrapped a white row to
    // (0x25, 0xff, 0xff).
    // Every expectation here is what -[CALayer renderInContext:] produced for the same tree on 25G76;
 // the measurement log
    TEST(LayerCompositor, MasksTheLayerAndItsSubtreeByTheMaskAlpha)
    {
        const auto composite = [](macos_layer_tree& tree, canvas& target) {
            return macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
        };

        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 8, 4}, {0, 0}, {0, 0});
            auto& red = make(tree, 2, {0, 0, 8, 4}, {0, 0}, {0, 0});
            red.background = rgba(1, 0, 0, 1);
            tree.add_sublayer(1, 2);

            auto& mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
            mask.background = rgba(1, 1, 1, 1);
            tree.set_mask(2, 3);

            canvas target{8, 4};
            const auto stats = composite(tree, target);
            EXPECT_EQ(stats.masks_applied, 1u);
            EXPECT_EQ(target.argb(1, 1), 0xFFFF0000u) << "inside the mask the layer draws unchanged";
            EXPECT_EQ(target.argb(6, 1), 0x00000000u) << "outside the mask nothing is written";
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 4, 4}, {0, 0}, {0, 0});
            auto& red = make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
            red.background = rgba(1, 0, 0, 1);
            tree.add_sublayer(1, 2);

            auto& mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
            mask.background = rgba(0, 0, 0, 0.5);
            tree.set_mask(2, 3);

            canvas target{4, 4};
            composite(tree, target);
            EXPECT_EQ(target.argb(1, 1), 0x80800000u) << "the mask contributes alpha only; an opaque black mask masks in fully";
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 8, 4}, {0, 0}, {0, 0});
            make(tree, 2, {0, 0, 8, 4}, {0, 0}, {0, 0});
            auto& child = make(tree, 4, {0, 0, 8, 4}, {0, 0}, {0, 0});
            child.background = rgba(0, 0, 1, 1);
            tree.add_sublayer(1, 2);
            tree.add_sublayer(2, 4);

            auto& mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
            mask.background = rgba(1, 1, 1, 1);
            tree.set_mask(2, 3);

            canvas target{8, 4};
            composite(tree, target);
            EXPECT_EQ(target.argb(1, 1), 0xFF0000FFu) << "a mask on an ancestor clips its descendants";
            EXPECT_EQ(target.argb(6, 1), 0x00000000u);
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 8, 4}, {0, 0}, {0, 0});
            auto& red = make(tree, 2, {0, 0, 8, 4}, {0, 0}, {0, 0});
            red.background = rgba(1, 0, 0, 1);
            red.sublayer_transform = macos_layer_affine::translation(4, 0);
            tree.add_sublayer(1, 2);

            auto& mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
            mask.background = rgba(1, 1, 1, 1);
            tree.set_mask(2, 3);

            canvas target{8, 4};
            composite(tree, target);
            EXPECT_EQ(target.argb(1, 1), 0x00000000u) << "the masked layer's sublayerTransform moves the mask";
            EXPECT_EQ(target.argb(6, 1), 0xFFFF0000u);
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 8, 4}, {0, 0}, {0, 0});
            auto& red = make(tree, 2, {0, 0, 8, 4}, {0, 0}, {0, 0});
            red.background = rgba(1, 0, 0, 1);
            tree.add_sublayer(1, 2);

            make(tree, 3, {0, 0, 8, 4}, {0, 0}, {0, 0});
            auto& shape = make(tree, 5, {0, 0, 4, 4}, {0, 0}, {0, 0});
            shape.background = rgba(1, 1, 1, 1);
            tree.add_sublayer(3, 5);
            tree.set_mask(2, 3);

            canvas target{8, 4};
            composite(tree, target);
            EXPECT_EQ(target.argb(1, 1), 0xFFFF0000u) << "a mask's own sublayers contribute its alpha";
            EXPECT_EQ(target.argb(6, 1), 0x00000000u);
        }
        {
            macos_layer_tree tree{};
            make(tree, 1, {0, 0, 4, 4}, {0, 0}, {0, 0});
            make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
            tree.add_sublayer(1, 2);

            auto& outer_mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
            outer_mask.background = rgba(1, 1, 1, 0.5);
            tree.set_mask(2, 3);

            auto& inner = make(tree, 4, {0, 0, 4, 4}, {0, 0}, {0, 0});
            inner.background = rgba(1, 0, 0, 1);
            tree.add_sublayer(2, 4);
            auto& inner_mask = make(tree, 5, {0, 0, 4, 4}, {0, 0}, {0, 0});
            inner_mask.background = rgba(1, 1, 1, 0.5);
            tree.set_mask(4, 5);

            canvas target{4, 4};
            const auto stats = composite(tree, target);
            EXPECT_EQ(stats.masks_applied, 2u);
            EXPECT_EQ(target.argb(1, 1), 0x40400000u) << "nested masks multiply: renderInContext: measured 40 40 0a 00";
        }
    }

    // Both fill rules, against -[CAShapeLayer renderInContext:] on 25G76 for the same two nested
    // rectangles in a 16x16 layer (src/tools/macos-gui-probe/render.m): even-odd leaves the inner
    // square empty and non-zero fills it, and both are crisp because the edges land on pixel bounds.
    TEST(LayerCompositor, FillsAGeneralPathUnderBothWindingRules)
    {
        const auto ring = [] {
            std::vector<sogen::macos_layer_path_edge> edges{};
            const auto rectangle = [&](const double x0, const double y0, const double x1, const double y1) {
                edges.push_back({{x0, y0}, {x1, y0}});
                edges.push_back({{x1, y0}, {x1, y1}});
                edges.push_back({{x1, y1}, {x0, y1}});
                edges.push_back({{x0, y1}, {x0, y0}});
            };

            rectangle(1, 1, 15, 15);
            rectangle(5, 5, 11, 11);
            return edges;
        }();

        for (const auto even_odd : {false, true})
        {
            macos_layer_tree tree{};
            auto& node = make(tree, 1, {0, 0, 16, 16}, {0, 0}, {0, 0});
            node.shape.kind = sogen::macos_layer_shape_kind::path;
            node.shape.fill = rgba(1, 1, 1, 1);
            node.shape.even_odd = even_odd;
            node.shape.edges = ring;

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.shapes_filled, 1u);
            EXPECT_EQ(target.argb(2, 2), 0xFFFFFFFFu) << "the outer rectangle fills under either rule";
            EXPECT_EQ(target.argb(0, 0), 0x00000000u) << "and nothing outside it is touched";
            EXPECT_EQ(target.argb(8, 8), even_odd ? 0x00000000u : 0xFFFFFFFFu)
                << "the inner rectangle is a hole only under the even-odd rule";
        }
    }

    // A general path takes the same 4x4 supersampled edge the closed forms take, so a diagonal is
    // graded rather than cut. The exact edge value is not asserted: a 45-degree hypotenuse passes
    // through the sample grid's own points, where the supersampled answer is degenerate.
    TEST(LayerCompositor, AntialiasesAGeneralPathsDiagonal)
    {
        macos_layer_tree tree{};
        auto& node = make(tree, 1, {0, 0, 16, 16}, {0, 0}, {0, 0});
        node.shape.kind = sogen::macos_layer_shape_kind::path;
        node.shape.fill = rgba(1, 1, 1, 1);
        node.shape.edges = {
            {{2, 2}, {14, 2}},
            {{14, 2}, {2, 14}},
            {{2, 14}, {2, 2}},
        };

        canvas target{16, 16};
        macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
        EXPECT_EQ(target.argb(3, 3), 0xFFFFFFFFu) << "well inside the hypotenuse the fill is solid";
        EXPECT_EQ(target.argb(14, 3), 0x00000000u) << "and outside it nothing is written";

        const auto edge = target.argb(12, 3) >> 24;
        EXPECT_GT(edge, 0u) << "the pixel the hypotenuse crosses is graded, not cut";
        EXPECT_LT(edge, 255u);
    }

    TEST(LayerCompositor, DropsAMaskItCannotEvaluateRatherThanErasingTheLayer)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 4, 4}, {0, 0}, {0, 0});
        auto& red = make(tree, 2, {0, 0, 4, 4}, {0, 0}, {0, 0});
        red.background = rgba(1, 0, 0, 1);
        tree.add_sublayer(1, 2);

        auto& mask = make(tree, 3, {0, 0, 4, 4}, {0, 0}, {0, 0});
        mask.background = rgba(1, 1, 1, 1);
        mask.contents = {sogen::macos_layer_contents_kind::unresolved, 0xC0FFEE, {}};
        tree.set_mask(2, 3);

        canvas target{4, 4};
        const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
        EXPECT_EQ(stats.masks_skipped, 1u);
        EXPECT_EQ(stats.masks_applied, 0u);
        EXPECT_EQ(target.argb(1, 1), 0xFFFF0000u) << "an unreadable mask leaves the layer visible rather than erasing it";
    }

    // A CAPortalLayer's projection is measured (spec section 11) and deliberately not applied: A/B'd
    // twice on Calculator, the second time with matchesPosition folded in, honouring it cost seven of
    // the sixteen contents blits and truncated the keypad. Counting it is the honest state.
    TEST(LayerCompositor, CountsAPortalRatherThanGuessingWhereItDraws)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 8, 8}, {0, 0}, {0, 0});
        auto& source = make(tree, 2, {0, 0, 2, 2}, {0, 0}, {0, 0});
        source.background = rgba(1, 0, 0, 1);
        tree.add_sublayer(1, 2);

        auto& portal = make(tree, 3, {0, 0, 4, 4}, {4, 4}, {0, 0});
        portal.portal_source = 2;
        portal.hides_source_layer = true;
        portal.portal_matches_position = true;
        tree.add_sublayer(1, 3);

        canvas target{8, 8};
        const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
        EXPECT_EQ(stats.portals_unmodelled, 1u);
        EXPECT_EQ(target.argb(0, 0), 0xFFFF0000u) << "hidesSourceLayer must not remove a layer sogen cannot reproject";
        EXPECT_EQ(target.argb(5, 5), 0x00000000u) << "and nothing is invented at the portal";
    }

    // The rasteriser itself is tested in its own suite; these cover the wiring -- which layers become a
    // field, where the elements' transforms come from, and which effects are refused. Contract:
    TEST(LayerCompositor, RendersACASDFLayerFieldFromItsElementSubtree)
    {
        const auto build = [](macos_layer_tree& tree, const sogen::macos_sdf_effect_kind kind, const bool backdrop) {
            make(tree, 1, {0, 0, 16, 16}, {0, 0}, {0, 0});
            auto& host = make(tree, 2, {0, 0, 16, 16}, {0, 0}, {0, 0});
            host.role = backdrop ? sogen::macos_layer_role::backdrop : sogen::macos_layer_role::plain;
            tree.add_sublayer(1, 2);

            auto& container = make(tree, 3, {0, 0, 16, 16}, {0, 0}, {0, 0});
            container.role = sogen::macos_layer_role::sdf_container;
            container.sdf.effect_kind = kind;
            container.sdf.effect_color = rgba(1, 0, 0, 1);
            tree.add_sublayer(2, 3);

            auto& element = make(tree, 4, {0, 0, 8, 8}, {4, 4}, {0, 0});
            element.role = sogen::macos_layer_role::sdf_element;
            element.sdf.operation = sogen::macos_sdf_operation::unite;
            tree.add_sublayer(3, 4);
        };

        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::fill, false);

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_drawn, 1u);
            EXPECT_EQ(stats.sdf_elements_used, 1u);
            EXPECT_EQ(target.argb(8, 8), 0xFFFF0000u) << "a CASDFFillEffect paints its own colour over the element";
            EXPECT_EQ(target.argb(1, 1), 0x00000000u) << "and nothing where the field does not reach";
        }
        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::output, false);

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_drawn, 0u);
            EXPECT_EQ(stats.sdf_fields_unmodelled, 1u) << "an output effect with no backdrop ancestor is refused, not invented";
            EXPECT_EQ(target.argb(8, 8), 0x00000000u);
        }
        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::output, true);

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_approximated, 1u) << "under a CABackdropLayer it takes the measured flat-glass shortcut";
            EXPECT_EQ(target.argb(8, 8), 0xFF2B2725u) << "and paints the chrome colour sampled from the real app, (43,39,37)";
        }
        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::gradient, false);

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_approximated, 1u);
            EXPECT_EQ(target.argb(8, 8), 0xFF343331u) << "a gradient-effect field is a key capsule, measured (52,51,49)";
        }
        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::fill, false);
            tree.find(4)->sdf.operation.reset();

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_drawn, 0u) << "an element whose operation string sogen cannot parse leaves the field empty";
        }
        {
            macos_layer_tree tree{};
            build(tree, sogen::macos_sdf_effect_kind::fill, false);
            tree.find(4)->hidden = true;

            canvas target{16, 16};
            const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
            EXPECT_EQ(stats.sdf_fields_drawn, 0u) << "a hidden element is not handed to the rasteriser at all";
        }
    }

    // SwiftUI puts the glyph cells before the bezel under one SDFLayer, so a field drawn where the
    // CASDFLayer sits lands on top of them; the contract's "one fill of the whole subtree" is taken to
    // mean the group draws it on the way in, behind everything the group contains.
    TEST(LayerCompositor, ASwiftUIGroupDrawsItsFieldBeforeItsContent)
    {
        macos_layer_tree tree{};
        make(tree, 1, {0, 0, 16, 16}, {0, 0}, {0, 0});
        auto& group = make(tree, 2, {0, 0, 16, 16}, {0, 0}, {0, 0});
        group.role = sogen::macos_layer_role::sdf_group;
        tree.add_sublayer(1, 2);

        auto& glyph = make(tree, 3, {0, 0, 16, 16}, {0, 0}, {0, 0});
        glyph.background = rgba(0, 0, 1, 1);
        tree.add_sublayer(2, 3);

        auto& container = make(tree, 4, {0, 0, 16, 16}, {0, 0}, {0, 0});
        container.role = sogen::macos_layer_role::sdf_container;
        container.sdf.effect_kind = sogen::macos_sdf_effect_kind::fill;
        container.sdf.effect_color = rgba(1, 0, 0, 1);
        tree.add_sublayer(2, 4);

        auto& element = make(tree, 5, {0, 0, 8, 8}, {4, 4}, {0, 0});
        element.role = sogen::macos_layer_role::sdf_element;
        element.sdf.operation = sogen::macos_sdf_operation::unite;
        tree.add_sublayer(4, 5);

        canvas target{16, 16};
        const auto stats = macos_layer_composite(tree, 1, target.surface(), {.flip_y = false}, {});
        EXPECT_EQ(stats.sdf_fields_drawn, 1u);
        EXPECT_EQ(target.argb(8, 8), 0xFF0000FFu) << "the later glyph sibling still wins: the field went down first";
    }

    TEST(LayerCompositor, SaturatesRatherThanWrappingAnOverBrightSource)
    {
        uint8_t white[4] = {255, 255, 255, 255};
        sogen::macos_layer_blend_pixel(white, 255, 217, 217, 217);
        EXPECT_EQ(white[0], 255);
        EXPECT_EQ(white[1], 255);
        EXPECT_EQ(white[2], 255);
        EXPECT_EQ(white[3], 255);

        uint8_t opaque[4] = {255, 255, 255, 255};
        sogen::macos_layer_blend_pixel(opaque, 200, 200, 200, 1);
        EXPECT_EQ(opaque[0], 255) << "200 + 254/255 of 255 is 454, which must not become 198";
    }

    TEST(LayerCompositor, QuantisesChannelsTheWayCoreGraphicsDoes)
    {
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(0.0), 0);
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(1.0), 255);
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(0.5), 128) << "127.5 rounds up, as CoreGraphics does";
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(-1.0), 0);
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(2.0), 255);
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(std::nan("")), 0);
        EXPECT_EQ(sogen::macos_layer_channel_to_byte(0.098039215686274508), 25);
    }

    TEST(LayerCompositor, ComposesAndInvertsAffinesConsistently)
    {
        const macos_layer_affine first{1.5, 0.25, -0.5, 2.0, 7.0, -3.0};
        const macos_layer_affine second{0.5, -1.25, 3.0, 0.75, -2.0, 11.0};
        const auto combined = first.then(second);

        const macos_layer_point probes[] = {{0, 0}, {1, 0}, {0, 1}, {-13.5, 4.25}};
        for (const auto probe : probes)
        {
            const auto direct = combined.apply(probe);
            const auto stepwise = second.apply(first.apply(probe));
            EXPECT_NEAR(direct.x, stepwise.x, 1e-9);
            EXPECT_NEAR(direct.y, stepwise.y, 1e-9);

            const auto inverse = combined.inverse();
            ASSERT_TRUE(inverse.has_value());
            const auto round_trip = inverse->apply(direct);
            EXPECT_NEAR(round_trip.x, probe.x, 1e-9);
            EXPECT_NEAR(round_trip.y, probe.y, 1e-9);
        }

        EXPECT_FALSE(macos_layer_affine::scaling(0.0, 1.0).inverse().has_value());
        EXPECT_FALSE((macos_layer_affine{1, 2, 2, 4, 0, 0}).inverse().has_value());
        EXPECT_TRUE(macos_layer_affine{}.is_identity());
        EXPECT_FALSE(macos_layer_affine::translation(1, 0).is_identity());
    }
}
