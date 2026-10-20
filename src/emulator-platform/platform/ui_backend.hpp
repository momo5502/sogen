#pragma once

#include "window.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{

    struct ui_insets
    {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    struct ui_window_desc
    {
        hwnd handle{};
        hwnd parent{};
        hwnd owner{};
        RECT rect{};
        ui_insets client_insets{};
        std::u16string class_name{};
        std::u16string title{};
        uint32_t style{};
        uint32_t ex_style{};
        uint32_t control_id{};
        bool visible{};
        bool enabled{true};
        bool top_level{};
    };

    enum class ui_surface_format
    {
        bgra8,
        rgba8,
    };

    struct ui_surface_desc
    {
        int width{};
        int height{};
        int stride{};
        ui_surface_format format{ui_surface_format::bgra8};
        const void* pixels{};
    };

    struct ui_extent
    {
        uint32_t width{};
        uint32_t height{};
    };

    struct ui_rect
    {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    struct ui_event
    {
        hwnd window{};
        uint32_t message{};
        uint64_t wParam{};
        uint64_t lParam{};

        // Set on WM_MOUSEMOVE while the host reports relative motion: the host cursor does not move, so the
        // emulator moves the guest cursor by this delta instead of taking the position from lParam.
        std::optional<std::pair<int32_t, int32_t>> relative_motion{};
    };

    class ui_backend
    {
      public:
        using event_sink = std::function<void(const ui_event&)>;

        virtual ~ui_backend() = default;

        virtual void set_event_sink(event_sink sink) = 0;
        virtual void pump_events() = 0;

        virtual void reset()
        {
        }

        virtual void create_window(const ui_window_desc& /*desc*/)
        {
        }

        virtual void destroy_window(const hwnd /*window*/)
        {
        }

        virtual void set_window_rect(const hwnd /*window*/, const RECT& /*rect*/)
        {
        }

        virtual void set_window_visible(const hwnd /*window*/, const bool /*visible*/)
        {
        }

        virtual void set_window_enabled(const hwnd /*window*/, const bool /*enabled*/)
        {
        }

        virtual void set_window_title(const hwnd /*window*/, std::u16string_view /*title*/)
        {
        }

        virtual void invalidate(const hwnd /*window*/, const std::optional<RECT>& /*rect*/)
        {
        }

        virtual void present_surface(const hwnd /*window*/, const ui_surface_desc& /*surface*/)
        {
        }

        virtual std::vector<std::string> vulkan_instance_extensions()
        {
            return {};
        }

        virtual uint64_t create_vulkan_surface(const hwnd /*window*/, const uint64_t /*instance*/)
        {
            return 0;
        }

        // The extent a Vulkan swapchain on the window should have, for backends whose surface does not follow the
        // guest window's size by itself. std::nullopt keeps the extent the driver reports.
        virtual std::optional<ui_extent> vulkan_surface_extent(const hwnd /*window*/)
        {
            return std::nullopt;
        }

        // Warp the host cursor so it tracks a guest SetCursorPos. Without this, the next host mouse-motion
        // event reports the (unchanged) host position and overwrites the recentered guest cursor, making
        // relative-mouse deltas (in-game look) jump. screen_x/y are emulated-desktop screen coordinates.
        virtual void set_cursor_position(const hwnd /*window*/, const int32_t /*screen_x*/, const int32_t /*screen_y*/)
        {
        }

        // Show or hide the host cursor. Games that capture the mouse hide it (ShowCursor(FALSE) /
        // SetCursor(NULL)) and recenter every frame; without honoring this the host cursor stays visible
        // and flickers as the guest warps it back to the center.
        virtual void set_cursor_visibility(const bool /*visible*/)
        {
        }

        // Confine the host cursor to a guest ClipCursor rectangle (emulated screen coordinates), or release it.
        // With relative set, the host reports raw motion instead, which a hidden cursor needs: mouse-look keeps
        // receiving deltas at the edge of the clip rectangle, as raw input does on Windows.
        virtual void set_cursor_clip(const hwnd /*window*/, const std::optional<ui_rect>& /*screen_rect*/, const bool /*relative*/)
        {
        }
    };

    class null_ui_backend final : public ui_backend
    {
      public:
        void set_event_sink(event_sink /*sink*/) override
        {
        }

        void pump_events() override
        {
        }

        void reset() override
        {
        }
    };

    std::unique_ptr<ui_backend> create_default_ui_backend();
    std::unique_ptr<ui_backend> create_sdl_ui_backend();
    std::unique_ptr<ui_backend> create_web_ui_backend();

} // namespace sogen
