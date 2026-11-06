#pragma once

#include <platform/ui_backend.hpp>

namespace sogen
{
    class ui_event_router
    {
        struct state;

      public:
        class subscription
        {
          public:
            subscription() = default;
            subscription(subscription&&) noexcept = default;
            subscription& operator=(subscription&& other) noexcept;
            subscription(const subscription&) = delete;
            subscription& operator=(const subscription&) = delete;
            ~subscription();

            void reset();

          private:
            friend class ui_event_router;
            subscription(const std::shared_ptr<state>& state, hwnd window, const std::shared_ptr<ui_backend::event_sink>& handler);

            std::weak_ptr<state> state_{};
            std::weak_ptr<ui_backend::event_sink> handler_{};
            hwnd window_{};
        };

        ui_event_router();
        ~ui_event_router();
        ui_event_router(const ui_event_router&) = delete;
        ui_event_router& operator=(const ui_event_router&) = delete;
        ui_event_router(ui_event_router&&) = delete;
        ui_event_router& operator=(ui_event_router&&) = delete;

        [[nodiscard]] subscription subscribe(hwnd window, ui_backend::event_sink handler);
        bool dispatch(const ui_event& event);
        void clear();

      private:
        std::shared_ptr<state> state_;
    };
}
