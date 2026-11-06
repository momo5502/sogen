#include "std_include.hpp"
#include "ui_event_router.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace sogen
{
    struct ui_event_router::state
    {
        using handler_map = std::unordered_map<hwnd, std::shared_ptr<ui_backend::event_sink>>;

        // A handler may unregister itself while it is being called.
        std::recursive_mutex mutex{};
        handler_map handlers{};
    };

    ui_event_router::subscription::subscription(const std::shared_ptr<state>& state, const hwnd window,
                                                const std::shared_ptr<ui_backend::event_sink>& handler)
        : state_(state),
          handler_(handler),
          window_(window)
    {
    }

    ui_event_router::subscription& ui_event_router::subscription::operator=(subscription&& other) noexcept
    {
        if (this != &other)
        {
            this->reset();
            this->state_ = std::move(other.state_);
            this->handler_ = std::move(other.handler_);
            this->window_ = other.window_;
        }
        return *this;
    }

    ui_event_router::subscription::~subscription()
    {
        this->reset();
    }

    void ui_event_router::subscription::reset()
    {
        std::shared_ptr<ui_backend::event_sink> removed{};
        if (const auto state = this->state_.lock())
        {
            const std::scoped_lock lock(state->mutex);
            const auto entry = state->handlers.find(this->window_);
            if (entry != state->handlers.end() && entry->second == this->handler_.lock())
            {
                removed = std::move(entry->second);
                state->handlers.erase(entry);
            }
        }
        this->state_.reset();
        this->handler_.reset();
    }

    ui_event_router::ui_event_router()
        : state_(std::make_shared<state>())
    {
    }

    ui_event_router::~ui_event_router()
    {
        this->clear();
    }

    ui_event_router::subscription ui_event_router::subscribe(const hwnd window, ui_backend::event_sink handler)
    {
        if (window == 0 || !handler)
        {
            throw std::invalid_argument("A window and event handler are required");
        }

        const std::scoped_lock lock(this->state_->mutex);
        const auto entry = std::make_shared<ui_backend::event_sink>(std::move(handler));
        if (!this->state_->handlers.emplace(window, entry).second)
        {
            throw std::invalid_argument("The window already has an event handler");
        }
        return subscription{this->state_, window, entry};
    }

    bool ui_event_router::dispatch(const ui_event& event)
    {
        const auto state = this->state_;
        const std::scoped_lock lock(state->mutex);
        const auto entry = state->handlers.find(event.window);
        if (entry == state->handlers.end())
        {
            return false;
        }

        const auto handler = entry->second;
        (*handler)(event);
        return true;
    }

    void ui_event_router::clear()
    {
        state::handler_map removed{};
        {
            const std::scoped_lock lock(this->state_->mutex);
            removed.swap(this->state_->handlers);
        }
    }
}
