#pragma once

#include <platform/ui_backend.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ANativeWindow;

namespace sogen::android
{
    class ui_host
    {
      public:
        virtual ~ui_host() = default;

        virtual void on_window_created(hwnd window, int32_t width, int32_t height, std::u16string_view title, bool visible) = 0;
        virtual void on_window_destroyed(hwnd window) = 0;
        virtual void on_window_resized(hwnd window, int32_t width, int32_t height) = 0;
        virtual void on_window_visibility_changed(hwnd window, bool visible) = 0;
        virtual void on_window_title_changed(hwnd window, std::u16string_view title) = 0;
        // The guest moved the cursor itself, e.g. with SetCursorPos; x and y are in the window's client coordinates.
        virtual void on_cursor_moved(hwnd window, int32_t x, int32_t y) = 0;
        // The guest confined and hid the cursor of [window], whose input should then move the mouse by deltas.
        virtual void on_relative_mouse_changed(hwnd window, bool relative) = 0;
    };

    enum class mouse_button : uint8_t
    {
        left,
        right,
        middle,
    };

    class android_ui_backend final : public ui_backend
    {
      public:
        explicit android_ui_backend(ui_host& host);
        ~android_ui_backend() override;

        android_ui_backend(const android_ui_backend&) = delete;
        android_ui_backend& operator=(const android_ui_backend&) = delete;
        android_ui_backend(android_ui_backend&&) = delete;
        android_ui_backend& operator=(android_ui_backend&&) = delete;

        void attach_surface(hwnd window, ANativeWindow* surface);
        void detach_surface(hwnd window);

        void post_mouse_move(hwnd window, int32_t x, int32_t y);
        void post_mouse_motion(hwnd window, int32_t dx, int32_t dy);
        void post_mouse_button(hwnd window, mouse_button button, bool down, int32_t x, int32_t y);
        void post_mouse_wheel(hwnd window, int32_t delta, int32_t x, int32_t y);
        void post_key(hwnd window, int32_t keycode, bool down);
        void post_text(hwnd window, std::u16string_view text);
        void post_close(hwnd window);

        void set_event_sink(event_sink sink) override;
        void pump_events() override;
        void reset() override;

        void create_window(const ui_window_desc& desc) override;
        void destroy_window(hwnd window) override;
        void set_window_rect(hwnd window, const RECT& rect) override;
        void set_window_visible(hwnd window, bool visible) override;
        void set_window_title(hwnd window, std::u16string_view title) override;
        void present_surface(hwnd window, const ui_surface_desc& surface) override;
        void set_cursor_position(hwnd window, int32_t screen_x, int32_t screen_y) override;
        void set_cursor_clip(hwnd window, const std::optional<ui_rect>& screen_rect, bool relative) override;

        std::vector<std::string> vulkan_instance_extensions() override;
        uint64_t create_vulkan_surface(hwnd window, uint64_t instance) override;
        std::optional<ui_extent> vulkan_surface_extent(hwnd window) override;

      private:
        struct window_state
        {
            ui_window_desc desc{};
            ANativeWindow* surface{};
            bool vulkan_owned{};
        };

        struct delayed_event
        {
            ui_event event{};
            std::chrono::steady_clock::time_point due{};
        };

        ui_host* host_{};
        void* vulkan_get_instance_proc_addr_{};
        event_sink sink_{};

        std::mutex mutex_{};
        std::condition_variable surface_attached_{};
        std::unordered_map<hwnd, window_state> windows_{};
        std::vector<ui_event> events_{};
        std::vector<delayed_event> delayed_events_{};
        std::unordered_map<uint8_t, std::chrono::steady_clock::time_point> key_down_since_{};
        hwnd active_window_{};
        uint16_t mouse_buttons_{};
        hwnd relative_window_{};

        void queue_event(hwnd window, uint32_t message, uint64_t w_param, uint64_t l_param);
        void activate(hwnd window);
        hwnd top_level_ancestor(hwnd window) const;
        window_state* find_top_level(hwnd window);
        void release_surfaces();
    };
}
