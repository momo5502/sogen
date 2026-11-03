#include "emulation_test_utils.hpp"

#include <ui_backends/surface_alpha.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t bgra(const uint8_t alpha, const uint8_t red, const uint8_t green, const uint8_t blue)
        {
            return (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(red) << 16) | (static_cast<uint32_t>(green) << 8) | blue;
        }
    }

    TEST(SurfaceAlphaTest, WithoutSourceAlphaTheConstantAlphaIsUsed)
    {
        // What GDI leaves behind: opaque colours with alpha 0, which must not make the window vanish.
        std::array pixels{bgra(0, 0xFF, 0x00, 0x00), bgra(0, 0x80, 0x80, 0x80)};

        apply_layered_blend(pixels, false, 128);

        EXPECT_EQ(bgra(128, 128, 0, 0), pixels[0]);
        EXPECT_EQ(bgra(128, 64, 64, 64), pixels[1]);
    }

    TEST(SurfaceAlphaTest, WithoutSourceAlphaAFullConstantKeepsTheColours)
    {
        std::array pixels{bgra(0, 0x10, 0x20, 0x30)};

        apply_layered_blend(pixels, false, 255);

        EXPECT_EQ(bgra(255, 0x10, 0x20, 0x30), pixels[0]);
    }

    TEST(SurfaceAlphaTest, PremultipliedPixelsAreScaledByTheConstantAlpha)
    {
        std::array pixels{bgra(0xFF, 0xFF, 0x00, 0x00), bgra(0x00, 0x00, 0x00, 0x00), bgra(0x80, 0x80, 0x40, 0x20)};

        apply_layered_blend(pixels, true, 128);

        EXPECT_EQ(bgra(128, 128, 0, 0), pixels[0]);
        EXPECT_EQ(bgra(0, 0, 0, 0), pixels[1]);
        EXPECT_EQ(bgra(64, 64, 32, 16), pixels[2]);
    }

    TEST(SurfaceAlphaTest, PremultipliedPixelsSurviveAFullConstantAlpha)
    {
        std::array pixels{bgra(0x00, 0x00, 0x00, 0x00), bgra(0x80, 0x40, 0x20, 0x10), bgra(0xFF, 0xFF, 0xFF, 0xFF)};
        const auto original = pixels;

        apply_layered_blend(pixels, true, 255);

        EXPECT_EQ(original, pixels);
    }
}
