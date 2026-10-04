#include <gtest/gtest.h>

#include <gui/macos_sdf_field.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The expected numbers in this file are not re-derivations of the rasteriser's own arithmetic: they are
// what the real QuartzCore composited on macOS 25G76, captured with CGWindowListCreateImage by
// src/tools/macos-gui-probe/sdfrender.m and tabulated in
//. Every case
// below reproduces one 120x90-point window captured at scale 2, i.e. a 240x180 device-pixel frame.

namespace
{
    using sogen::macos_layer_affine;
    using sogen::macos_layer_color;
    using sogen::macos_layer_point;
    using sogen::macos_layer_rect;
    using sogen::macos_layer_surface;
    using sogen::macos_sdf_coverage_raster;
    using sogen::macos_sdf_diagnostic_kind;
    using sogen::macos_sdf_effect;
    using sogen::macos_sdf_effect_kind;
    using sogen::macos_sdf_element;
    using sogen::macos_sdf_field;
    using sogen::macos_sdf_mode;
    using sogen::macos_sdf_operation;
    using sogen::macos_sdf_stats;

    constexpr int32_t CAPTURE_WIDTH = 240;
    constexpr int32_t CAPTURE_HEIGHT = 180;
    constexpr int32_t MID_ROW = 90;
    constexpr int32_t MID_COLUMN = 120;
    constexpr double CAPTURE_SCALE = 2.0;

    macos_layer_affine capture_transform()
    {
        return macos_layer_affine::scaling(CAPTURE_SCALE, CAPTURE_SCALE);
    }

    struct canvas
    {
        std::vector<uint8_t> bytes{};
        int32_t width{};
        int32_t height{};

        explicit canvas(const int32_t w = CAPTURE_WIDTH, const int32_t h = CAPTURE_HEIGHT)
            : bytes(static_cast<size_t>(w) * static_cast<size_t>(h) * 4),
              width(w),
              height(h)
        {
            this->clear_to_black();
        }

        void clear_to_black()
        {
            for (size_t index = 0; index < this->bytes.size(); index += 4)
            {
                this->bytes[index + 0] = 0;
                this->bytes[index + 1] = 0;
                this->bytes[index + 2] = 0;
                this->bytes[index + 3] = 0xFF;
            }
        }

        macos_layer_surface surface()
        {
            return {this->bytes.data(), this->width, this->height, this->width * 4};
        }

        uint32_t rgb(const int32_t x, const int32_t y) const
        {
            const auto offset = (static_cast<size_t>(y) * static_cast<size_t>(this->width) + static_cast<size_t>(x)) * 4;
            return (static_cast<uint32_t>(this->bytes[offset + 2]) << 16) | (static_cast<uint32_t>(this->bytes[offset + 1]) << 8) |
                   static_cast<uint32_t>(this->bytes[offset + 0]);
        }

        uint8_t red(const int32_t x, const int32_t y) const
        {
            return this->bytes[(static_cast<size_t>(y) * static_cast<size_t>(this->width) + static_cast<size_t>(x)) * 4 + 2];
        }
    };

    using run = std::pair<int32_t, int32_t>;

    // "50% coverage" in the capture tables is a red channel of at least 0x80: the rasteriser rounds a
    // coverage of exactly 0.5 to 128, matching the 0x80 the host wrote for both half-covered pixels.
    std::vector<run> runs_of(const canvas& frame, const bool along_row, const int32_t fixed)
    {
        std::vector<run> found{};
        const auto limit = along_row ? frame.width : frame.height;

        for (int32_t index = 0; index < limit; ++index)
        {
            const auto covered = (along_row ? frame.red(index, fixed) : frame.red(fixed, index)) >= 0x80;
            if (covered)
            {
                if (found.empty() || found.back().second != index - 1)
                {
                    found.emplace_back(index, index);
                }
                else
                {
                    found.back().second = index;
                }
            }
        }

        return found;
    }

    macos_sdf_element element(const double x, const double y, const double width, const double height, const double corner_radius,
                              const macos_sdf_operation operation = macos_sdf_operation::unite)
    {
        macos_sdf_element ready{};
        ready.bounds = {0.0, 0.0, width, height};
        ready.corner_radius = corner_radius;
        ready.to_container = macos_layer_affine::translation(x, y);
        ready.operation = operation;
        return ready;
    }

    macos_sdf_effect fill(const double r, const double g, const double b, const double a)
    {
        macos_sdf_effect effect{};
        effect.kind = macos_sdf_effect_kind::fill;
        effect.class_name = "CASDFFillEffect";
        effect.color = macos_layer_color{true, r, g, b, a};
        return effect;
    }

    macos_sdf_effect red_fill()
    {
        return fill(1.0, 0.0, 0.0, 1.0);
    }

    macos_sdf_stats paint(canvas& frame, const macos_sdf_field& field, const macos_sdf_effect& effect = red_fill())
    {
        return macos_sdf_render(field, effect, capture_transform(), frame.surface());
    }

    macos_sdf_field baseline_capsule()
    {
        macos_sdf_field field{};
        field.elements.push_back(element(20.0, 20.0, 80.0, 50.0, 25.0));
        return field;
    }

    macos_sdf_field two_circles(const double first_x, const double second_x, const double smoothness)
    {
        macos_sdf_field field{};
        field.elements.push_back(element(first_x, 25.0, 40.0, 40.0, 20.0));
        field.elements.push_back(element(second_x, 25.0, 40.0, 40.0, 20.0));
        field.smoothness = smoothness;
        return field;
    }

    bool has_detail(const macos_sdf_stats& stats, const std::string_view name, const std::string_view fragment)
    {
        const auto* found = stats.diagnostic(name);
        return found != nullptr && found->detail.find(fragment) != std::string::npos;
    }
}

TEST(SdfFieldTest, ModeAndOperationAreStringsNotIntegers)
{
    EXPECT_EQ(sogen::macos_sdf_mode_from_name("bounds"), macos_sdf_mode::bounds);
    EXPECT_EQ(sogen::macos_sdf_mode_from_name("contents"), macos_sdf_mode::contents);
    EXPECT_EQ(sogen::macos_sdf_operation_from_name("union"), macos_sdf_operation::unite);
    EXPECT_EQ(sogen::macos_sdf_operation_from_name("subtraction"), macos_sdf_operation::subtract);

    EXPECT_FALSE(sogen::macos_sdf_mode_from_name("0").has_value());
    EXPECT_FALSE(sogen::macos_sdf_mode_from_name("Bounds").has_value());
    EXPECT_FALSE(sogen::macos_sdf_mode_from_name("").has_value());
    EXPECT_FALSE(sogen::macos_sdf_operation_from_name("1").has_value());
    EXPECT_FALSE(sogen::macos_sdf_operation_from_name("intersection").has_value());
    EXPECT_FALSE(sogen::macos_sdf_operation_from_name("smoothUnion").has_value());
}

TEST(SdfFieldTest, EffectClassNamesMapToTheNineMeasuredEffects)
{
    EXPECT_EQ(sogen::macos_sdf_effect_kind_from_class_name("CASDFFillEffect"), macos_sdf_effect_kind::fill);
    EXPECT_EQ(sogen::macos_sdf_effect_kind_from_class_name("CASDFOutputEffect"), macos_sdf_effect_kind::output);
    EXPECT_EQ(sogen::macos_sdf_effect_kind_from_class_name("CASDFVisualizationEffect"), macos_sdf_effect_kind::visualization);
    EXPECT_EQ(sogen::macos_sdf_effect_kind_from_class_name("CASDFKeyFillHighlightEffect"), macos_sdf_effect_kind::key_fill_highlight);
    EXPECT_FALSE(sogen::macos_sdf_effect_kind_from_class_name("CASDFEffect").has_value());
    EXPECT_FALSE(sogen::macos_sdf_effect_kind_from_class_name("CAFilter").has_value());
}

// Section 3.1: the corner radius is not clamped to half the shorter side. An 80x50 box with r=100 has a
// half-width of sqrt(100^2 - 75^2) - 60 = 6.14 at its centre row and a half-height of
// sqrt(100^2 - 60^2) - 75 = 5.00 at its centre column; the host measured 6.1 and 5.0.
TEST(SdfFieldTest, RoundBoxRadiusIsNotClamped)
{
    const macos_layer_rect bounds{20.0, 20.0, 80.0, 50.0};

    const auto half_width = std::sqrt(100.0 * 100.0 - 75.0 * 75.0) - 60.0;
    const auto half_height = std::sqrt(100.0 * 100.0 - 60.0 * 60.0) - 75.0;
    EXPECT_NEAR(half_width, 6.14, 0.01);
    EXPECT_NEAR(half_height, 5.0, 1e-9);

    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({60.0 + half_width, 45.0}, bounds, 100.0), 0.0, 1e-9);
    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({60.0 - half_width, 45.0}, bounds, 100.0), 0.0, 1e-9);
    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({60.0, 45.0 + half_height}, bounds, 100.0), 0.0, 1e-9);
    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({60.0, 45.0 - half_height}, bounds, 100.0), 0.0, 1e-9);

    // A clamped radius would draw the whole 80x50 capsule, which contains this point.
    EXPECT_GT(sogen::macos_sdf_round_box_distance({30.0, 45.0}, bounds, 100.0), 0.0);

    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({20.0, 45.0}, bounds, 25.0), 0.0, 1e-9);
    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({20.0, 20.0}, bounds, 0.0), 0.0, 1e-9);
    EXPECT_NEAR(sogen::macos_sdf_round_box_distance({60.0, 45.0}, bounds, 0.0), -25.0, 1e-9);
}

// Section 8.1, the primary reference rendering.
TEST(SdfFieldTest, SingleCapsuleReferenceScanlines)
{
    canvas frame{};
    const auto stats = paint(frame, baseline_capsule());

    EXPECT_EQ(stats.elements_used, 1u);
    EXPECT_TRUE(stats.diagnostics.empty());

    for (int32_t x = 0; x <= 39; ++x)
    {
        // The capture reads 020000 at x=39, which the contract records as colour-management rounding and
        // not coverage: the pixel centre sits 0.25 pt outside, so the ramp gives it exactly zero.
        EXPECT_EQ(frame.rgb(x, MID_ROW), 0x000000u) << "dev x " << x;
    }

    // The capture reads ff0000 across 40..199, but dev x 40 and 199 are the only two pixels of the row
    // whose exact coverage is not 1: the row is sampled at point y 45.25, a quarter point off the
    // capsule's centre line, so the semicircular cap puts their centres 0.2487 pt inside rather than
    // 0.25, for a coverage of 0.99748 and a red of 254. The host rounded that up, the same 2/255 of
    // colour management the contract flags at dev x 39.
    EXPECT_GE(frame.red(40, MID_ROW), 0xFE);
    EXPECT_GE(frame.red(199, MID_ROW), 0xFE);
    EXPECT_EQ(frame.rgb(40, MID_ROW) & 0x00FFFFu, 0u);
    EXPECT_EQ(frame.rgb(199, MID_ROW) & 0x00FFFFu, 0u);

    for (int32_t x = 41; x <= 198; ++x)
    {
        EXPECT_EQ(frame.rgb(x, MID_ROW), 0xFF0000u) << "dev x " << x;
    }

    for (int32_t x = 200; x < CAPTURE_WIDTH; ++x)
    {
        EXPECT_EQ(frame.rgb(x, MID_ROW), 0x000000u) << "dev x " << x;
    }

    for (int32_t y = 0; y <= 39; ++y)
    {
        EXPECT_EQ(frame.rgb(MID_COLUMN, y), 0x000000u) << "dev y " << y;
    }

    for (int32_t y = 40; y <= 139; ++y)
    {
        EXPECT_EQ(frame.rgb(MID_COLUMN, y), 0xFF0000u) << "dev y " << y;
    }

    for (int32_t y = 140; y < CAPTURE_HEIGHT; ++y)
    {
        EXPECT_EQ(frame.rgb(MID_COLUMN, y), 0x000000u) << "dev y " << y;
    }
}

// Section 4: four 60x50 sharp-cornered elements whose left edge sits at point 20.00, 20.25, 20.50 and
// 20.75. The host wrote ff0000 / 800a08 / 000000 / 800a08 at dev x=40; the 0a and 08 are colour
// management rounding 50% red over black, which the contract states is not a signal, so only the red
// channel is a coverage measurement.
TEST(SdfFieldTest, CoverageIsAHalfPixelRampAtQuarterPixelEdges)
{
    struct expectation
    {
        double edge{};
        uint8_t at_40{};
        uint8_t at_41{};
    };

    const expectation cases[]{
        {20.00, 0xFF, 0xFF},
        {20.25, 0x80, 0xFF},
        {20.50, 0x00, 0xFF},
        {20.75, 0x00, 0x80},
    };

    for (const auto& expected : cases)
    {
        canvas frame{};
        macos_sdf_field field{};
        field.elements.push_back(element(expected.edge, 20.0, 60.0, 50.0, 0.0));
        paint(frame, field);

        EXPECT_EQ(frame.red(39, MID_ROW), 0x00) << "edge " << expected.edge;
        EXPECT_EQ(frame.red(40, MID_ROW), expected.at_40) << "edge " << expected.edge;
        EXPECT_EQ(frame.red(41, MID_ROW), expected.at_41) << "edge " << expected.edge;
        EXPECT_EQ(frame.rgb(40, MID_ROW) & 0x00FFFFu, 0u) << "edge " << expected.edge;
    }
}

// Section 8.2, every row the measurements pin down. The four tangent-circle smoothness rows are the
// contract's stated gap and are covered separately below.
TEST(SdfFieldTest, CaseMatrixFiftyPercentRuns)
{
    struct expectation
    {
        const char* name{};
        macos_sdf_field field{};
        std::vector<run> midrow{};
        std::vector<run> midcol{};
        bool check_midcol{true};
    };

    const auto capsule = baseline_capsule();

    auto radius = [](const double corner_radius) {
        macos_sdf_field field{};
        field.elements.push_back(element(20.0, 20.0, 80.0, 50.0, corner_radius));
        return field;
    };

    auto offset = [&capsule](const double effect_offset) {
        auto field = capsule;
        field.effect_offset = effect_offset;
        return field;
    };

    macos_sdf_field subtraction{};
    subtraction.elements.push_back(element(20.0, 20.0, 80.0, 50.0, 25.0));
    subtraction.elements.push_back(element(45.0, 30.0, 30.0, 30.0, 15.0, macos_sdf_operation::subtract));

    macos_sdf_field reversed{};
    reversed.elements.push_back(element(45.0, 30.0, 30.0, 30.0, 15.0, macos_sdf_operation::subtract));
    reversed.elements.push_back(element(20.0, 20.0, 80.0, 50.0, 25.0));

    macos_sdf_field union_sub_union = subtraction;
    union_sub_union.elements.push_back(element(52.0, 37.0, 16.0, 16.0, 8.0));

    macos_sdf_field disjoint{};
    disjoint.elements.push_back(element(10.0, 25.0, 40.0, 40.0, 20.0));
    disjoint.elements.push_back(element(70.0, 25.0, 40.0, 40.0, 20.0));

    macos_sdf_field outside{};
    outside.elements.push_back(element(90.0, 20.0, 80.0, 50.0, 25.0));

    macos_sdf_field inert_distances = capsule;
    inert_distances.elements[0].contents_zero_value_distance = -5.0;
    inert_distances.elements[0].contents_one_value_distance = 5.0;

    const std::vector<expectation> cases{
        {"baseline_capsule", capsule, {{40, 199}}, {{40, 139}}},
        {"radius0_rect", radius(0.0), {{40, 199}}, {{40, 139}}},
        {"radius10", radius(10.0), {{40, 199}}, {{40, 139}}},
        {"radius20_circular", radius(20.0), {{40, 199}}, {{40, 139}}},
        {"radius_huge", radius(100.0), {{108, 131}}, {{80, 99}}},
        {"effect_offset_8", offset(8.0), {{56, 183}}, {{56, 123}}},
        {"effect_offset_-8", offset(-8.0), {{24, 215}}, {{24, 155}}},
        {"subtraction", subtraction, {{40, 89}, {150, 199}}, {{40, 59}, {120, 139}}},
        {"sub_then_union", reversed, {{40, 199}}, {{40, 139}}},
        {"union_sub_union", union_sub_union, {{40, 89}, {104, 135}, {150, 199}}, {{40, 59}, {74, 105}, {120, 139}}},
        {"two_disjoint", disjoint, {{20, 99}, {140, 219}}, {}, false},
        {"touch_sm00", two_circles(20.0, 60.0, 0.0), {{40, 199}}, {{84, 95}}},
        {"touch_sm02", two_circles(20.0, 60.0, 2.0), {{40, 199}}, {{81, 98}}},
        {"gap10_merge0", two_circles(15.0, 65.0, 0.0), {{30, 109}, {130, 209}}, {}, false},
        {"gap10_smooth16", two_circles(15.0, 65.0, 16.0), {{30, 111}, {128, 209}}, {}, false},
        {"gap4_sm08", two_circles(18.0, 62.0, 8.0), {{36, 203}}, {{89, 90}}},
        {"overlap10_sm00", two_circles(25.0, 55.0, 0.0), {{50, 189}}, {{63, 116}}},
        {"hidden_element", macos_sdf_field{}, {}, {}},
        {"zero_dist_-5_one_5", inert_distances, {{40, 199}}, {{40, 139}}},
        {"element_outside_bounds", outside, {{180, 239}}, {}, false},
    };

    for (const auto& expected : cases)
    {
        canvas frame{};
        paint(frame, expected.field);

        EXPECT_EQ(runs_of(frame, true, MID_ROW), expected.midrow) << expected.name << " midrow";
        if (expected.check_midcol)
        {
            EXPECT_EQ(runs_of(frame, false, MID_COLUMN), expected.midcol) << expected.name << " midcol";
        }
    }
}

// Section 5.1: a subtraction that runs before anything has been accumulated is a no-op, so the fold is
// ordered rather than a set operation on a collected shape.
TEST(SdfFieldTest, OrderOfTheFoldTurnsAHoleIntoNoHole)
{
    macos_sdf_field with_hole{};
    with_hole.elements.push_back(element(20.0, 20.0, 80.0, 50.0, 25.0));
    with_hole.elements.push_back(element(45.0, 30.0, 30.0, 30.0, 15.0, macos_sdf_operation::subtract));

    macos_sdf_field reversed{};
    reversed.elements.push_back(with_hole.elements[1]);
    reversed.elements.push_back(with_hole.elements[0]);

    canvas holed{};
    canvas solid{};
    paint(holed, with_hole);
    paint(solid, reversed);

    EXPECT_EQ(runs_of(holed, true, MID_ROW).size(), 2u);
    EXPECT_EQ(runs_of(solid, true, MID_ROW).size(), 1u);
    EXPECT_EQ(holed.rgb(MID_COLUMN, MID_ROW), 0x000000u);
    EXPECT_EQ(solid.rgb(MID_COLUMN, MID_ROW), 0xFF0000u);
}

// Section 5.4: d' = d + effectOffset, an erosion of exactly that many points on every side.
TEST(SdfFieldTest, EffectOffsetErodesAndDilatesByPoints)
{
    for (const auto shift : {8.0, -8.0})
    {
        auto field = baseline_capsule();
        field.effect_offset = shift;

        canvas frame{};
        paint(frame, field);

        const auto row = runs_of(frame, true, MID_ROW);
        const auto column = runs_of(frame, false, MID_COLUMN);
        ASSERT_EQ(row.size(), 1u);
        ASSERT_EQ(column.size(), 1u);

        const auto device_shift = static_cast<int32_t>(shift * CAPTURE_SCALE);
        EXPECT_EQ(row[0], run(40 + device_shift, 199 - device_shift));
        EXPECT_EQ(column[0], run(40 + device_shift, 139 - device_shift));
    }
}

// Section 5.2: the polynomial smooth-min reproduces the host at k=0 and k=2 and at every measured gap
// configuration, including k=8 and k=16 where the two fields never meet inside the shape.
TEST(SdfFieldTest, SmoothMinMatchesTheHostAtTwoAndAtTheGapCases)
{
    struct expectation
    {
        const char* name{};
        macos_sdf_field field{};
        std::vector<run> runs{};
        bool along_row{};
        int32_t fixed{};
    };

    const std::vector<expectation> cases{
        {"touch_sm00", two_circles(20.0, 60.0, 0.0), {{84, 95}}, false, MID_COLUMN},
        {"touch_sm02", two_circles(20.0, 60.0, 2.0), {{81, 98}}, false, MID_COLUMN},
        {"gap10_smooth16", two_circles(15.0, 65.0, 16.0), {{30, 111}, {128, 209}}, true, MID_ROW},
        {"gap4_sm08 midrow", two_circles(18.0, 62.0, 8.0), {{36, 203}}, true, MID_ROW},
        {"gap4_sm08 midcol", two_circles(18.0, 62.0, 8.0), {{89, 90}}, false, MID_COLUMN},
    };

    for (const auto& expected : cases)
    {
        canvas frame{};
        paint(frame, expected.field);
        EXPECT_EQ(runs_of(frame, expected.along_row, expected.fixed), expected.runs) << expected.name;
    }

    canvas exact{};
    const auto exact_stats = paint(exact, two_circles(20.0, 60.0, 2.0));
    EXPECT_GT(exact_stats.smooth_blend_pixels, 0u);
    EXPECT_EQ(exact_stats.diagnostic("smoothness"), nullptr);
}

// Section 5.2 records the residual honestly: the polynomial over-predicts the junction of two tangent
// circles by 8% at k=8 and 25% at k=32. These are the host's runs against this rasteriser's, so the
// error is pinned rather than papered over, and the renderer names it.
TEST(SdfFieldTest, SmoothMinOverPredictsTheTangentCircleJunction)
{
    struct expectation
    {
        double smoothness{};
        run measured{};
        run rasterised{};
    };

    const expectation cases[]{
        {4.0, {78, 101}, {77, 102}},
        {8.0, {73, 106}, {72, 107}},
        {16.0, {67, 112}, {63, 116}},
        {32.0, {60, 119}, {51, 128}},
    };

    for (const auto& expected : cases)
    {
        canvas frame{};
        const auto stats = paint(frame, two_circles(20.0, 60.0, expected.smoothness));

        const auto column = runs_of(frame, false, MID_COLUMN);
        ASSERT_EQ(column.size(), 1u);
        EXPECT_EQ(column[0], expected.rasterised) << "smoothness " << expected.smoothness;
        EXPECT_NE(column[0], expected.measured) << "smoothness " << expected.smoothness;

        EXPECT_EQ(runs_of(frame, true, MID_ROW), (std::vector<run>{{40, 199}})) << "smoothness " << expected.smoothness;

        EXPECT_GT(stats.smooth_blend_pixels, 0u);
        const auto* residual = stats.diagnostic("smoothness");
        ASSERT_NE(residual, nullptr) << "smoothness " << expected.smoothness;
        EXPECT_EQ(residual->kind, macos_sdf_diagnostic_kind::approximated);
    }
}

// Section 5.2: SwiftUI sets smoothness 6 or 8 while placing 44x44 keys on a 50-point pitch, so each
// field is 3.0 at the midpoint of the gap and no bridge forms. The contract goes one step further and
// says a plain fmin reproduces the captured keypad byte for byte; the polynomial smooth-min does not
// quite, and this pins the difference: it is confined to the single device-pixel column immediately
// outside each facing edge, where it adds at most 5/255 of coverage the host does not show.
TEST(SdfFieldTest, SwiftUIKeypadSpacingNeverBridgesTheGap)
{
    macos_sdf_field keys{};
    keys.smoothness = 6.0;
    for (const auto x : {0.0, 50.0, 100.0})
    {
        keys.elements.push_back(element(x, 20.0, 44.0, 44.0, 12.0));
    }

    canvas blended{};
    const auto stats = paint(blended, keys);

    macos_sdf_field sharp = keys;
    sharp.smoothness = 0.0;
    canvas plain{};
    paint(plain, sharp);

    EXPECT_EQ(runs_of(blended, true, 84).size(), 3u);
    EXPECT_EQ(runs_of(plain, true, 84).size(), 3u);
    EXPECT_EQ(blended.rgb(94, 84), 0x000000u);
    EXPECT_EQ(blended.rgb(194, 84), 0x000000u);

    int32_t worst = 0;
    std::vector<int32_t> columns{};
    for (int32_t y = 0; y < blended.height; ++y)
    {
        for (int32_t x = 0; x < blended.width; ++x)
        {
            const auto delta = std::abs(static_cast<int32_t>(blended.red(x, y)) - static_cast<int32_t>(plain.red(x, y)));
            if (delta == 0)
            {
                continue;
            }

            worst = std::max(worst, delta);
            if (std::find(columns.begin(), columns.end(), x) == columns.end())
            {
                columns.push_back(x);
            }
        }
    }

    std::sort(columns.begin(), columns.end());
    EXPECT_EQ(columns, (std::vector<int32_t>{88, 99, 188, 199}));
    EXPECT_LE(worst, 5);

    EXPECT_GT(stats.smooth_blend_pixels, 0u);
    ASSERT_NE(stats.diagnostic("smoothness"), nullptr);
    EXPECT_EQ(stats.diagnostic("smoothness")->kind, macos_sdf_diagnostic_kind::approximated);
}

// Section 2: the field is not clipped to the container and ignores its masksToBounds, so the caller
// hands over the whole subtree's elements and the rasteriser paints wherever they reach.
TEST(SdfFieldTest, TheFieldIsNotClippedToTheContainer)
{
    macos_sdf_field field{};
    field.elements.push_back(element(90.0, 20.0, 80.0, 50.0, 25.0));

    canvas frame{};
    paint(frame, field);

    EXPECT_EQ(runs_of(frame, true, MID_ROW), (std::vector<run>{{180, 239}}));
    EXPECT_EQ(frame.rgb(239, MID_ROW), 0xFF0000u);
}

// Section 5.3: mergeElements chooses whether the effect runs per element or once on the combined field.
// With a flat fill the host is byte-identical either way, which is the only paint this rasteriser has.
TEST(SdfFieldTest, MergeElementsIsANoOpForAFlatFill)
{
    auto separate = two_circles(15.0, 65.0, 0.0);
    auto merged = separate;
    merged.merge_elements = true;

    canvas first{};
    canvas second{};
    const auto stats = paint(first, separate);
    paint(second, merged);

    EXPECT_EQ(first.bytes, second.bytes);
    EXPECT_TRUE(stats.diagnostics.empty());
}

// Section 6: nil renders byte-identically to CASDFVisualizationEffect on the host, an unclipped debug
// field that floods the surface. Both must draw nothing.
TEST(SdfFieldTest, NilAndVisualizationEffectsDrawNothing)
{
    canvas nil_frame{};
    macos_sdf_effect nil_effect{};
    const auto nil_stats = macos_sdf_render(baseline_capsule(), nil_effect, capture_transform(), nil_frame.surface());

    EXPECT_EQ(nil_stats.pixels_written, 0u);
    EXPECT_TRUE(nil_stats.diagnostics.empty());
    EXPECT_EQ(nil_frame.rgb(MID_COLUMN, MID_ROW), 0x000000u);

    canvas debug_frame{};
    macos_sdf_effect visualization{};
    visualization.kind = macos_sdf_effect_kind::visualization;
    visualization.class_name = "CASDFVisualizationEffect";
    const auto debug_stats = macos_sdf_render(baseline_capsule(), visualization, capture_transform(), debug_frame.surface());

    EXPECT_EQ(debug_stats.pixels_written, 0u);
    EXPECT_EQ(debug_frame.bytes, nil_frame.bytes);
    EXPECT_TRUE(has_detail(debug_stats, "effect", "not clipped"));
}

TEST(SdfFieldTest, UnmodelledEffectsAreRefusedByName)
{
    const std::pair<macos_sdf_effect_kind, const char*> refused[]{
        {macos_sdf_effect_kind::gradient, "CASDFGradientEffect"},
        {macos_sdf_effect_kind::gradient_contour, "CASDFGradientContourEffect"},
        {macos_sdf_effect_kind::shadow, "CASDFShadowEffect"},
        {macos_sdf_effect_kind::glass_highlight, "CASDFGlassHighlightEffect"},
        {macos_sdf_effect_kind::glass_displacement, "CASDFGlassDisplacementEffect"},
        {macos_sdf_effect_kind::key_fill_highlight, "CASDFKeyFillHighlightEffect"},
        {macos_sdf_effect_kind::output, "CASDFOutputEffect"},
    };

    for (const auto& [kind, name] : refused)
    {
        macos_sdf_effect effect{};
        effect.kind = kind;
        effect.class_name = name;

        canvas frame{};
        const auto stats = macos_sdf_render(baseline_capsule(), effect, capture_transform(), frame.surface());

        EXPECT_EQ(stats.pixels_written, 0u) << name;
        EXPECT_TRUE(has_detail(stats, "effect", name)) << name;
        EXPECT_EQ(frame.rgb(MID_COLUMN, MID_ROW), 0x000000u) << name;
    }

    macos_sdf_effect unknown{};
    unknown.kind = macos_sdf_effect_kind::unmodelled;
    unknown.class_name = "CASDFSomethingNewEffect";

    canvas frame{};
    const auto stats = macos_sdf_render(baseline_capsule(), unknown, capture_transform(), frame.surface());
    EXPECT_TRUE(has_detail(stats, "effect", "CASDFSomethingNewEffect"));
}

TEST(SdfFieldTest, AnUnreadableFillColourIsRefusedRatherThanDefaultedToWhite)
{
    macos_sdf_effect effect{};
    effect.kind = macos_sdf_effect_kind::fill;
    effect.class_name = "CASDFFillEffect";

    canvas frame{};
    const auto stats = macos_sdf_render(baseline_capsule(), effect, capture_transform(), frame.surface());

    EXPECT_EQ(stats.pixels_written, 0u);
    EXPECT_NE(stats.diagnostic("CASDFFillEffect.color"), nullptr);
    EXPECT_EQ(frame.rgb(MID_COLUMN, MID_ROW), 0x000000u);
}

// Section 3.2: the value-to-distance map is known but the sampling geometry was never measured, and the
// host renders nothing when the contents image is missing.
TEST(SdfFieldTest, ModeContentsIsRefusedByName)
{
    auto field = baseline_capsule();
    field.elements[0].mode = macos_sdf_mode::contents;

    canvas frame{};
    const auto stats = paint(frame, field);

    EXPECT_EQ(stats.elements_used, 0u);
    EXPECT_EQ(stats.elements_refused, 1u);
    EXPECT_EQ(stats.pixels_written, 0u);
    EXPECT_TRUE(has_detail(stats, "mode", "contents"));
}

// Section 5.4: gaussianRadius blurs the field. It is zero on every SwiftUI layer measured and was never
// characterised, so the field is rendered unblurred and the parameter is named.
TEST(SdfFieldTest, GaussianRadiusIsNamedAndTheFieldRendersUnblurred)
{
    auto field = baseline_capsule();
    field.gaussian_radius = 6.0;

    canvas blurred{};
    canvas sharp{};
    const auto stats = paint(blurred, field);
    paint(sharp, baseline_capsule());

    const auto* named = stats.diagnostic("gaussianRadius");
    ASSERT_NE(named, nullptr);
    EXPECT_EQ(named->kind, macos_sdf_diagnostic_kind::unmodelled);
    EXPECT_EQ(blurred.bytes, sharp.bytes);
}

// Section 2: element transforms are honoured, and a rotation maps a distance field without rescaling it.
TEST(SdfFieldTest, RotatedAndScaledElementsKeepTheirDistanceInContainerPoints)
{
    macos_sdf_field rotated{};
    auto& turned = rotated.elements.emplace_back(element(0.0, 0.0, 80.0, 50.0, 25.0));
    // A quarter turn: the 80x50 capsule becomes 50 wide and 80 tall, spanning points x 35..85, y 5..85.
    turned.to_container = macos_layer_affine{0.0, 1.0, -1.0, 0.0, 85.0, 5.0};

    canvas frame{};
    paint(frame, rotated);

    EXPECT_EQ(runs_of(frame, true, MID_ROW), (std::vector<run>{{70, 169}}));
    EXPECT_EQ(runs_of(frame, false, MID_COLUMN), (std::vector<run>{{10, 169}}));

    macos_sdf_field scaled{};
    auto& doubled = scaled.elements.emplace_back(element(0.0, 0.0, 40.0, 25.0, 12.5));
    doubled.to_container = macos_layer_affine::scaling(2.0, 2.0).then(macos_layer_affine::translation(20.0, 20.0));
    scaled.effect_offset = 8.0;

    canvas scaled_frame{};
    paint(scaled_frame, scaled);

    EXPECT_EQ(runs_of(scaled_frame, true, MID_ROW), (std::vector<run>{{56, 183}}));
    EXPECT_EQ(runs_of(scaled_frame, false, MID_COLUMN), (std::vector<run>{{56, 123}}));
}

TEST(SdfFieldTest, NonSimilarityMappingsAreRefusedByName)
{
    macos_sdf_field sheared{};
    auto& element_with_shear = sheared.elements.emplace_back(element(20.0, 20.0, 80.0, 50.0, 25.0));
    element_with_shear.to_container = macos_layer_affine{1.0, 0.0, 0.4, 1.0, 20.0, 20.0};

    canvas frame{};
    const auto element_stats = paint(frame, sheared);

    EXPECT_EQ(element_stats.elements_used, 0u);
    EXPECT_EQ(element_stats.pixels_written, 0u);
    EXPECT_NE(element_stats.diagnostic("CASDFElementLayer.transform"), nullptr);

    // A shear whose two columns are the same length, so only the orthogonality half of the similarity
    // test can reject it.
    macos_sdf_field skewed{};
    auto& element_with_skew = skewed.elements.emplace_back(element(20.0, 20.0, 80.0, 50.0, 25.0));
    element_with_skew.to_container = macos_layer_affine{1.0, 0.0, 0.5, std::sqrt(3.0) / 2.0, 20.0, 20.0};

    canvas skewed_frame{};
    const auto skewed_stats = paint(skewed_frame, skewed);

    EXPECT_EQ(skewed_stats.elements_used, 0u);
    EXPECT_EQ(skewed_stats.pixels_written, 0u);
    EXPECT_NE(skewed_stats.diagnostic("CASDFElementLayer.transform"), nullptr);

    for (const auto& device : {macos_layer_affine::scaling(2.0, 3.0), macos_layer_affine{2.0, 0.0, 1.0, std::sqrt(3.0), 0.0, 0.0}})
    {
        canvas device_frame{};
        const auto device_stats = macos_sdf_render(baseline_capsule(), red_fill(), device, device_frame.surface());

        EXPECT_EQ(device_stats.pixels_written, 0u);
        EXPECT_NE(device_stats.diagnostic("containerToDevice"), nullptr);
    }
}

// Section 8.3, the two rows of the effect table a flat fill produces.
TEST(SdfFieldTest, FillEffectColoursMatchTheEffectTable)
{
    canvas white{};
    paint(white, baseline_capsule(), fill(1.0, 1.0, 1.0, 1.0));

    canvas half{};
    paint(half, baseline_capsule(), fill(1.0, 1.0, 1.0, 0.5));

    for (const auto x : {30, 36})
    {
        EXPECT_EQ(white.rgb(x, MID_ROW), 0x000000u);
        EXPECT_EQ(half.rgb(x, MID_ROW), 0x000000u);
    }

    for (const auto x : {44, 50, 60, 80, 120})
    {
        EXPECT_EQ(white.rgb(x, MID_ROW), 0xFFFFFFu) << "dev x " << x;
        EXPECT_EQ(half.rgb(x, MID_ROW), 0x808080u) << "dev x " << x;
    }

    // dev x 40 is the cap pixel of SingleCapsuleReferenceScanlines: coverage 0.99748, one level short of
    // the ff and 80 the capture rounded to.
    EXPECT_EQ(white.rgb(40, MID_ROW), 0xFEFEFEu);
    EXPECT_EQ(half.rgb(40, MID_ROW), 0x7F7F7Fu);
}

TEST(SdfFieldTest, CoverageRasterMatchesThePaintedAlpha)
{
    std::vector<float> values(static_cast<size_t>(CAPTURE_WIDTH) * static_cast<size_t>(CAPTURE_HEIGHT), 1.0f);
    macos_sdf_coverage_raster raster{values.data(), CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_WIDTH};

    macos_sdf_field field{};
    field.elements.push_back(element(20.25, 20.0, 60.0, 50.0, 0.0));

    const auto stats = macos_sdf_rasterize_coverage(field, capture_transform(), raster);
    EXPECT_EQ(stats.elements_used, 1u);

    const auto at = [&](const int32_t x, const int32_t y) {
        return values[static_cast<size_t>(y) * CAPTURE_WIDTH + static_cast<size_t>(x)];
    };

    EXPECT_FLOAT_EQ(at(0, 0), 0.0f);
    EXPECT_FLOAT_EQ(at(39, MID_ROW), 0.0f);
    EXPECT_FLOAT_EQ(at(40, MID_ROW), 0.5f);
    EXPECT_FLOAT_EQ(at(41, MID_ROW), 1.0f);
    EXPECT_FLOAT_EQ(at(159, MID_ROW), 1.0f);
    EXPECT_FLOAT_EQ(at(160, MID_ROW), 0.5f);
    EXPECT_FLOAT_EQ(at(161, MID_ROW), 0.0f);
    EXPECT_FLOAT_EQ(at(239, MID_ROW), 0.0f);
}

TEST(SdfFieldTest, RenderingIsSourceOverAndSaturates)
{
    canvas frame{};
    for (size_t index = 0; index < frame.bytes.size(); index += 4)
    {
        frame.bytes[index + 0] = 0xFF;
        frame.bytes[index + 1] = 0xFF;
        frame.bytes[index + 2] = 0xFF;
        frame.bytes[index + 3] = 0xFF;
    }

    paint(frame, baseline_capsule(), fill(1.0, 0.0, 0.0, 0.5));

    EXPECT_EQ(frame.rgb(MID_COLUMN, MID_ROW), 0xFF7F7Fu);
    EXPECT_EQ(frame.rgb(0, 0), 0xFFFFFFu);
}

TEST(SdfFieldTest, MeasuredGlassFillIsTheSectionSevenMidpoint)
{
    const auto light = sogen::macos_sdf_measured_glass_fill(false);
    const auto dark = sogen::macos_sdf_measured_glass_fill(true);

    EXPECT_TRUE(light.present);
    EXPECT_NEAR(light.r * 255.0, 243.0, 1e-9);
    EXPECT_NEAR(light.a, 0.90, 1e-9);
    EXPECT_NEAR(dark.r * 255.0, 30.0, 1e-9);
    EXPECT_NEAR(dark.a, 0.75, 1e-9);
}
