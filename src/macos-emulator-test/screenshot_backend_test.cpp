#include <gtest/gtest.h>

#include <screenshot_ui_backend.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
    sogen::screenshot_ui_backend& as_screenshot(const std::unique_ptr<sogen::ui_backend>& backend)
    {
        return *static_cast<sogen::screenshot_ui_backend*>(backend.get());
    }

    std::vector<uint8_t> solid(const int width, const int height, const uint8_t c0, const uint8_t c1, const uint8_t c2,
                               const uint8_t alpha = 0xFF)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u, 0);
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i] = c0;
            pixels[i + 1] = c1;
            pixels[i + 2] = c2;
            pixels[i + 3] = alpha;
        }

        return pixels;
    }

    sogen::ui_window_desc window_at(const sogen::hwnd handle, const int x, const int y, const int w, const int h)
    {
        return sogen::ui_window_desc{
            .handle = handle,
            .rect = {.left = x, .top = y, .right = x + w, .bottom = y + h},
            .visible = true,
        };
    }

    TEST(ScreenshotBackend, ComposesAWindowOntoTheDesktopAtItsRect)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(64, 48);
        shot.set_background(0x10, 0x20, 0x30);
        shot.create_window(window_at(1, 8, 6, 16, 12));

        const auto pixels = solid(16, 12, 0xFF, 0x3B, 0x30);
        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 16, .height = 12, .stride = 16 * 4, .format = sogen::ui_surface_format::rgba8, .pixels = pixels.data()});

        const auto image = shot.compose();
        ASSERT_EQ(image.width, 64);
        ASSERT_EQ(image.height, 48);

        EXPECT_EQ(image.pixel_at(0, 0), (std::array<uint8_t, 4>{0x10, 0x20, 0x30, 0xFF})) << "background outside the window";
        EXPECT_EQ(image.pixel_at(8, 6), (std::array<uint8_t, 4>{0xFF, 0x3B, 0x30, 0xFF})) << "the window's top-left lands at its rect";
        EXPECT_EQ(image.pixel_at(23, 17), (std::array<uint8_t, 4>{0xFF, 0x3B, 0x30, 0xFF})) << "and its bottom-right";
        EXPECT_EQ(image.pixel_at(24, 18), (std::array<uint8_t, 4>{0x10, 0x20, 0x30, 0xFF})) << "one pixel past it is background again";
        EXPECT_EQ(shot.present_count(), 1u);
    }

    // A guest hands over BGRA far more often than RGBA -- it is what CGBitmapContext produces with
    // kCGImageAlphaPremultipliedFirst on little-endian -- so getting the swap wrong would silently turn
    // every red window blue.
    TEST(ScreenshotBackend, SwapsRedAndBlueForBgraSurfaces)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(8, 8);
        shot.create_window(window_at(1, 0, 0, 4, 4));

        const auto pixels = solid(4, 4, 0x30, 0x3B, 0xFF);
        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 4, .height = 4, .stride = 4 * 4, .format = sogen::ui_surface_format::bgra8, .pixels = pixels.data()});

        EXPECT_EQ(shot.compose().pixel_at(1, 1), (std::array<uint8_t, 4>{0xFF, 0x3B, 0x30, 0xFF}));
    }

    TEST(ScreenshotBackend, HonoursStridePaddingRatherThanCopyingIt)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(8, 8);
        shot.create_window(window_at(1, 0, 0, 2, 2));

        constexpr int stride = 4 * 4;
        std::vector<uint8_t> pixels(static_cast<size_t>(stride) * 2, 0xAA);
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 2; ++x)
            {
                auto* p = pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                p[0] = 0x11;
                p[1] = 0x22;
                p[2] = 0x33;
                p[3] = 0xFF;
            }
        }

        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 2, .height = 2, .stride = stride, .format = sogen::ui_surface_format::rgba8, .pixels = pixels.data()});

        const auto image = shot.compose();
        EXPECT_EQ(image.pixel_at(0, 0), (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0xFF}));
        EXPECT_EQ(image.pixel_at(1, 1), (std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0xFF})) << "row 1 starts at the stride, not at the width";
    }

    TEST(ScreenshotBackend, LaterWindowsPaintOverEarlierOnes)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(16, 16);
        shot.create_window(window_at(1, 0, 0, 8, 8));
        shot.create_window(window_at(2, 4, 4, 8, 8));

        const auto back = solid(8, 8, 0x00, 0xFF, 0x00);
        const auto front = solid(8, 8, 0xFF, 0x00, 0x00);

        shot.present_surface(1,
                             sogen::ui_surface_desc{
                                 .width = 8, .height = 8, .stride = 32, .format = sogen::ui_surface_format::rgba8, .pixels = back.data()});
        shot.present_surface(2,
                             sogen::ui_surface_desc{
                                 .width = 8, .height = 8, .stride = 32, .format = sogen::ui_surface_format::rgba8, .pixels = front.data()});

        const auto image = shot.compose();
        EXPECT_EQ(image.pixel_at(1, 1), (std::array<uint8_t, 4>{0x00, 0xFF, 0x00, 0xFF})) << "only the first window covers this";
        EXPECT_EQ(image.pixel_at(5, 5), (std::array<uint8_t, 4>{0xFF, 0x00, 0x00, 0xFF})) << "the later window wins where they overlap";
    }

    TEST(ScreenshotBackend, BlendsTranslucentPixelsOverWhatIsBehind)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(8, 8);
        shot.set_background(0x00, 0x00, 0x00);
        shot.create_window(window_at(1, 0, 0, 4, 4));

        const auto pixels = solid(4, 4, 0xFF, 0xFF, 0xFF, 0x80);
        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 4, .height = 4, .stride = 16, .format = sogen::ui_surface_format::rgba8, .pixels = pixels.data()});

        const auto composed = shot.compose().pixel_at(1, 1);
        EXPECT_NEAR(composed[0], 0x80, 2) << "half-opaque white over black is mid grey";
        EXPECT_EQ(composed[3], 0xFF) << "the desktop itself stays opaque";
    }

    TEST(ScreenshotBackend, HiddenAndDestroyedWindowsDoNotAppear)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(16, 16);
        shot.set_background(0x00, 0x00, 0x00);
        shot.create_window(window_at(1, 0, 0, 8, 8));

        const auto pixels = solid(8, 8, 0xFF, 0x00, 0x00);
        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 8, .height = 8, .stride = 32, .format = sogen::ui_surface_format::rgba8, .pixels = pixels.data()});

        ASSERT_EQ(shot.compose().pixel_at(1, 1)[0], 0xFF);

        shot.set_window_visible(1, false);
        EXPECT_EQ(shot.compose().pixel_at(1, 1)[0], 0x00);

        shot.set_window_visible(1, true);
        shot.destroy_window(1);
        EXPECT_EQ(shot.compose().pixel_at(1, 1)[0], 0x00);
        EXPECT_EQ(shot.windows().size(), 0u);
    }

    TEST(ScreenshotBackend, WritesAPngThatStartsWithTheSignature)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(32, 16);

        const auto path = std::filesystem::temp_directory_path() / "sogen-screenshot-test.png";
        std::filesystem::remove(path);

        ASSERT_TRUE(shot.write(path));
        ASSERT_TRUE(std::filesystem::exists(path));

        std::ifstream file(path, std::ios::binary);
        std::array<char, 8> signature{};
        file.read(signature.data(), signature.size());

        EXPECT_EQ(signature, (std::array<char, 8>{'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'}));
        std::filesystem::remove(path);
    }

    TEST(ScreenshotBackend, PostedEventsReachTheSinkOnlyWhenPumped)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        std::vector<uint32_t> seen{};
        shot.set_event_sink([&](const sogen::ui_event& event) { seen.push_back(event.message); });

        shot.post_event(sogen::ui_event{.window = 1, .message = 0x0201});
        EXPECT_TRUE(seen.empty()) << "posting does not deliver";

        shot.pump_events();
        ASSERT_EQ(seen.size(), 1u);
        EXPECT_EQ(seen[0], 0x0201u);

        shot.pump_events();
        EXPECT_EQ(seen.size(), 1u) << "an event is delivered once";
    }

    // A window that has been declared opaque is composited whatever its alpha channel says. That is what
    // the word means, and it is the difference between a window whose backing store is still all zeroes
    // showing as black and showing as nothing at all -- which reads as "no window was created".
    TEST(ScreenshotBackend, AnOpaqueWindowIsCompositedRegardlessOfItsAlpha)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(8, 8);
        shot.set_background(0x40, 0x40, 0x40);
        shot.create_window(window_at(1, 0, 0, 4, 4));

        const auto transparent = solid(4, 4, 0x00, 0x00, 0x00, 0x00);
        shot.present_surface(
            1, sogen::ui_surface_desc{
                   .width = 4, .height = 4, .stride = 16, .format = sogen::ui_surface_format::rgba8, .pixels = transparent.data()});

        EXPECT_EQ(shot.compose().pixel_at(1, 1), (std::array<uint8_t, 4>{0x40, 0x40, 0x40, 0xFF}))
            << "a translucent window with nothing drawn into it leaves the background alone";

        shot.set_window_opaque(1, true);
        EXPECT_EQ(shot.compose().pixel_at(1, 1), (std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0xFF}))
            << "declared opaque, the same pixels cover it";
    }

    // The desktop itself is never transparent, whatever a window's own alpha byte holds: it is the thing
    // The one thing that separates this backend in the analyzer from the same backend in the browser: the
    // page posts real pointer and key events into it, and nobody posts anything into a screenshot run.
    TEST(ScreenshotBackend, HasNoInputSourceUntilAFrontEndDeclaresOne)
    {
        const auto backend = sogen::create_screenshot_ui_backend();

        EXPECT_FALSE(backend->can_deliver_input()) << "a composed screenshot has nobody behind it to click";

        as_screenshot(backend).set_input_source(true);
        EXPECT_TRUE(backend->can_deliver_input());
    }

    // being written to a PNG, and a hole in it would read as a hole in the image.
    TEST(ScreenshotBackend, TheComposedDesktopStaysOpaque)
    {
        const auto backend = sogen::create_screenshot_ui_backend();
        auto& shot = as_screenshot(backend);

        shot.set_desktop_size(8, 8);
        shot.create_window(window_at(1, 0, 0, 4, 4));
        shot.set_window_opaque(1, true);

        const auto clear = solid(4, 4, 0x11, 0x22, 0x33, 0x00);
        shot.present_surface(1,
                             sogen::ui_surface_desc{
                                 .width = 4, .height = 4, .stride = 16, .format = sogen::ui_surface_format::rgba8, .pixels = clear.data()});

        const auto image = shot.compose();
        EXPECT_EQ(image.pixel_at(1, 1)[3], 0xFF);
        EXPECT_EQ(image.pixel_at(6, 6)[3], 0xFF);
    }
}
