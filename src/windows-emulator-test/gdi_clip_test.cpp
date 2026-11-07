#include <gtest/gtest.h>

#include <gdi_clip.hpp>

#include <map>
#include <utility>
#include <vector>

namespace sogen::test
{
    namespace
    {
        // How many times each pixel is visited; a correct region visits every visible pixel exactly once.
        std::map<std::pair<LONG, LONG>, int> visited_pixels(const gdi::clip_region& region, const RECT& area)
        {
            std::map<std::pair<LONG, LONG>, int> visits{};
            region.for_each_span(area, [&](const LONG x_begin, const LONG x_end, const LONG y) {
                for (auto x = x_begin; x < x_end; ++x)
                {
                    ++visits[{x, y}];
                }
            });
            return visits;
        }

        bool inside(const RECT& rect, const LONG x, const LONG y)
        {
            return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
        }
    }

    TEST(GdiClipRegionTest, VisitsExactlyTheVisiblePixelsOnce)
    {
        constexpr RECT client{.left = 0, .top = 0, .right = 12, .bottom = 10};
        constexpr RECT child{.left = 3, .top = 2, .right = 8, .bottom = 6};
        constexpr RECT overlapping_sibling{.left = 6, .top = 4, .right = 15, .bottom = 9};
        constexpr RECT area{.left = 1, .top = 1, .right = 11, .bottom = 9};

        gdi::clip_region region{client};
        region.subtract(child);
        region.subtract(overlapping_sibling);

        const auto visits = visited_pixels(region, area);
        for (LONG y = -1; y < 12; ++y)
        {
            for (LONG x = -1; x < 16; ++x)
            {
                const bool visible = inside(client, x, y) && !inside(child, x, y) && !inside(overlapping_sibling, x, y);
                const auto it = visits.find({x, y});
                const int count = it == visits.end() ? 0 : it->second;
                EXPECT_EQ(visible && inside(area, x, y) ? 1 : 0, count) << "pixel " << x << "," << y;
                EXPECT_EQ(visible, region.contains(x, y)) << "pixel " << x << "," << y;
            }
        }
    }

    TEST(GdiClipRegionTest, IntersectionKeepsOnlyTheCommonArea)
    {
        gdi::clip_region region = gdi::clip_region::whole(20, 20);
        region.subtract({.left = 5, .top = 0, .right = 10, .bottom = 20});
        region.intersect({.left = 3, .top = 3, .right = 8, .bottom = 6});

        const auto visits = visited_pixels(region, {.left = 0, .top = 0, .right = 20, .bottom = 20});
        EXPECT_EQ(6u, visits.size());
        for (LONG y = 3; y < 6; ++y)
        {
            for (LONG x = 3; x < 5; ++x)
            {
                EXPECT_EQ(1u, visits.count({x, y})) << "pixel " << x << "," << y;
            }
        }
    }

    TEST(GdiClipRegionTest, FragmentedRegionsStillVisitEachVisiblePixelOnce)
    {
        constexpr RECT client{.left = 0, .top = 0, .right = 40, .bottom = 40};
        gdi::clip_region region{client};
        std::vector<RECT> holes{};
        for (LONG y = 2; y < 40; y += 8)
        {
            for (LONG x = 2; x < 40; x += 8)
            {
                holes.push_back({.left = x, .top = y, .right = x + 3, .bottom = y + 3});
                region.subtract(holes.back());
            }
        }
        ASSERT_GT(region.rects().size(), 16u);

        const auto visits = visited_pixels(region, client);
        for (LONG y = 0; y < 40; ++y)
        {
            for (LONG x = 0; x < 40; ++x)
            {
                const bool visible = std::ranges::none_of(holes, [&](const RECT& hole) { return inside(hole, x, y); });
                const auto it = visits.find({x, y});
                EXPECT_EQ(visible ? 1 : 0, it == visits.end() ? 0 : it->second) << "pixel " << x << "," << y;
            }
        }
    }
}
