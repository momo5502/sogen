#pragma once

#include <platform/ui_backend.hpp>

#include <array>
#include <filesystem>
#include <vector>

namespace sogen
{
    struct screenshot_window
    {
        hwnd handle{};
        RECT rect{};
        std::u16string title{};
        bool visible{};
        bool opaque{};
        int width{};
        int height{};
        std::vector<uint8_t> rgba{};
    };

    struct screenshot_image
    {
        int width{};
        int height{};
        std::vector<uint8_t> rgba{};

        std::array<uint8_t, 4> pixel_at(int x, int y) const;
    };

    // A ui_backend with no display behind it. Every window's last presented surface is kept, and compose()
    // paints them onto a desktop bitmap. This is the only way anything in the tree can assert on pixels:
    // the SDL backend needs a screen and the web backend needs a browser, so neither can be a test's eyes.
    class screenshot_ui_backend final : public ui_backend
    {
      public:
        void set_event_sink(event_sink sink) override;
        void pump_events() override;
        bool can_deliver_input() const override;
        void reset() override;
        void create_window(const ui_window_desc& desc) override;
        void destroy_window(hwnd window) override;
        void set_window_rect(hwnd window, const RECT& rect) override;
        void set_window_visible(hwnd window, bool visible) override;
        void set_window_opaque(hwnd window, bool opaque);
        void set_window_title(hwnd window, std::u16string_view title) override;
        void present_surface(hwnd window, const ui_surface_desc& surface) override;

        void set_desktop_size(int width, int height);
        void set_background(uint8_t r, uint8_t g, uint8_t b);

        // Off by default. The analyzer composes headlessly with nobody to click, so a guest that runs out
        // of work there has finished; the browser front-end posts real pointer and key events into this
        // same backend and turns it on, which is what stops an idle GUI app being read as a deadlock.
        void set_input_source(bool live);

        size_t present_count() const
        {
            return this->present_count_;
        }

        const std::vector<screenshot_window>& windows() const
        {
            return this->windows_;
        }

        const screenshot_window* find_window(hwnd window) const;

        screenshot_image compose() const;
        bool write(const std::filesystem::path& path) const;

        void post_event(const ui_event& event);

      private:
        screenshot_window* find_mutable(hwnd window);

        std::vector<screenshot_window> windows_{};
        event_sink sink_{};
        std::vector<ui_event> pending_{};

        bool input_source_{false};
        int desktop_width_{1440};
        int desktop_height_{900};
        std::array<uint8_t, 3> background_{0x1E, 0x1E, 0x22};
        size_t present_count_{};
    };

    std::unique_ptr<ui_backend> create_screenshot_ui_backend();
}
