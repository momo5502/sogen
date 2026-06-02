#pragma once

#include "window.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sogen
{

    struct ui_insets
    {
        int left{};
        int top{};
        int right{};
        int bottom{};
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

    struct ui_event
    {
        hwnd window{};
        uint32_t message{};
        uint64_t wParam{};
        uint64_t lParam{};
    };

    class ui_backend
    {
      public:
        using event_sink = std::function<void(const ui_event&)>;

        virtual ~ui_backend() = default;

        virtual void set_event_sink(event_sink sink) = 0;
        virtual void pump_events() = 0;

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
    };

    std::unique_ptr<ui_backend> create_default_ui_backend();
    std::unique_ptr<ui_backend> create_win32_ui_backend();
    std::unique_ptr<ui_backend> create_sdl_ui_backend();
    std::unique_ptr<ui_backend> create_web_ui_backend();

} // namespace sogen
