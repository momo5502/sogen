#include "emulation_test_utils.hpp"

#include <ui_backends/window_geometry.hpp>

namespace sogen::test
{
    namespace
    {
        ui_window_desc make_desc()
        {
            ui_window_desc desc{};
            desc.rect = RECT{.left = 100, .top = 200, .right = 500, .bottom = 600};
            desc.client_insets = ui_insets{.left = 8, .top = 31, .right = 8, .bottom = 8};
            return desc;
        }
    }

    TEST(WindowGeometryTest, ConversionFollowsTheWindowOrigin)
    {
        auto desc = make_desc();

        const auto [before_x, before_y] = screen_point_in_window(desc, 150, 300);
        EXPECT_FLOAT_EQ(150.0f - 100.0f - 8.0f, before_x);
        EXPECT_FLOAT_EQ(300.0f - 200.0f - 31.0f, before_y);

        move_window_origin(desc.rect, 1000, 1200);

        const auto [after_x, after_y] = screen_point_in_window(desc, 150, 300);
        EXPECT_FLOAT_EQ(150.0f - 1000.0f - 8.0f, after_x);
        EXPECT_FLOAT_EQ(300.0f - 1200.0f - 31.0f, after_y);
    }

    TEST(WindowGeometryTest, MovingTheOriginKeepsTheSize)
    {
        auto desc = make_desc();
        const auto width = desc.rect.right - desc.rect.left;
        const auto height = desc.rect.bottom - desc.rect.top;

        move_window_origin(desc.rect, -40000, 40000);

        EXPECT_EQ(-40000, desc.rect.left);
        EXPECT_EQ(40000, desc.rect.top);
        EXPECT_EQ(width, desc.rect.right - desc.rect.left);
        EXPECT_EQ(height, desc.rect.bottom - desc.rect.top);
    }

    TEST(WindowGeometryTest, ConversionUsesOriginsOutsideThe16BitRange)
    {
        auto desc = make_desc();
        move_window_origin(desc.rect, 40000, -40000);

        const auto [x, y] = screen_point_in_window(desc, 40100, -39900);
        EXPECT_FLOAT_EQ(100.0f - 8.0f, x);
        EXPECT_FLOAT_EQ(100.0f - 31.0f, y);
    }
}
