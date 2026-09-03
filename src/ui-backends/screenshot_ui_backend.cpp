#include "std_include.hpp"
#include "screenshot_ui_backend.hpp"

#include "png_writer.hpp"

#include <algorithm>
#include <cstring>

namespace sogen
{
    namespace
    {
        constexpr int MAX_DESKTOP_DIMENSION = static_cast<int>(PNG_MAX_DIMENSION);

        int rect_width(const RECT& rect)
        {
            return static_cast<int>(rect.right - rect.left);
        }

        int rect_height(const RECT& rect)
        {
            return static_cast<int>(rect.bottom - rect.top);
        }
    }

    std::array<uint8_t, 4> screenshot_image::pixel_at(const int x, const int y) const
    {
        if (x < 0 || y < 0 || x >= this->width || y >= this->height)
        {
            return {};
        }

        const auto offset = (static_cast<size_t>(y) * static_cast<size_t>(this->width) + static_cast<size_t>(x)) * 4u;
        if (offset + 4 > this->rgba.size())
        {
            return {};
        }

        return {this->rgba[offset], this->rgba[offset + 1], this->rgba[offset + 2], this->rgba[offset + 3]};
    }

    void screenshot_ui_backend::set_event_sink(event_sink sink)
    {
        this->sink_ = std::move(sink);
    }

    void screenshot_ui_backend::pump_events()
    {
        if (!this->sink_)
        {
            this->pending_.clear();
            return;
        }

        // Drained into a local first: a sink is allowed to post in response to what it is handed, and
        // iterating the member while it grows would invalidate the iterator.
        auto events = std::move(this->pending_);
        this->pending_.clear();

        for (const auto& event : events)
        {
            this->sink_(event);
        }
    }

    void screenshot_ui_backend::reset()
    {
        this->windows_.clear();
        this->pending_.clear();
        this->present_count_ = 0;
    }

    void screenshot_ui_backend::create_window(const ui_window_desc& desc)
    {
        if (this->find_mutable(desc.handle) != nullptr)
        {
            return;
        }

        this->windows_.push_back(screenshot_window{
            .handle = desc.handle,
            .rect = desc.rect,
            .title = desc.title,
            .visible = desc.visible,
        });
    }

    void screenshot_ui_backend::destroy_window(const hwnd window)
    {
        std::erase_if(this->windows_, [window](const screenshot_window& entry) { return entry.handle == window; });
    }

    void screenshot_ui_backend::set_window_rect(const hwnd window, const RECT& rect)
    {
        if (auto* entry = this->find_mutable(window))
        {
            entry->rect = rect;
        }
    }

    void screenshot_ui_backend::set_window_visible(const hwnd window, const bool visible)
    {
        if (auto* entry = this->find_mutable(window))
        {
            entry->visible = visible;
        }
    }

    void screenshot_ui_backend::set_window_opaque(const hwnd window, const bool opaque)
    {
        if (auto* entry = this->find_mutable(window))
        {
            entry->opaque = opaque;
        }
    }

    void screenshot_ui_backend::set_window_title(const hwnd window, const std::u16string_view title)
    {
        if (auto* entry = this->find_mutable(window))
        {
            entry->title.assign(title);
        }
    }

    void screenshot_ui_backend::present_surface(const hwnd window, const ui_surface_desc& surface)
    {
        auto* entry = this->find_mutable(window);
        if (entry == nullptr || surface.pixels == nullptr || surface.width <= 0 || surface.height <= 0)
        {
            return;
        }

        const auto row_bytes = static_cast<size_t>(surface.width) * 4u;
        const auto stride = surface.stride > 0 ? static_cast<size_t>(surface.stride) : row_bytes;
        if (stride < row_bytes)
        {
            return;
        }

        entry->width = surface.width;
        entry->height = surface.height;
        entry->rgba.assign(row_bytes * static_cast<size_t>(surface.height), 0);

        const auto* source = static_cast<const uint8_t*>(surface.pixels);
        const auto swap_red_and_blue = surface.format == ui_surface_format::bgra8;

        for (int y = 0; y < surface.height; ++y)
        {
            const auto* in = source + static_cast<size_t>(y) * stride;
            auto* out = entry->rgba.data() + static_cast<size_t>(y) * row_bytes;

            if (!swap_red_and_blue)
            {
                std::memcpy(out, in, row_bytes);
                continue;
            }

            for (int x = 0; x < surface.width; ++x)
            {
                out[x * 4 + 0] = in[x * 4 + 2];
                out[x * 4 + 1] = in[x * 4 + 1];
                out[x * 4 + 2] = in[x * 4 + 0];
                out[x * 4 + 3] = in[x * 4 + 3];
            }
        }

        ++this->present_count_;
    }

    void screenshot_ui_backend::set_desktop_size(const int width, const int height)
    {
        this->desktop_width_ = std::clamp(width, 1, MAX_DESKTOP_DIMENSION);
        this->desktop_height_ = std::clamp(height, 1, MAX_DESKTOP_DIMENSION);
    }

    void screenshot_ui_backend::set_background(const uint8_t r, const uint8_t g, const uint8_t b)
    {
        this->background_ = {r, g, b};
    }

    void screenshot_ui_backend::set_input_source(const bool live)
    {
        this->input_source_ = live;
    }

    bool screenshot_ui_backend::can_deliver_input() const
    {
        return this->input_source_;
    }

    const screenshot_window* screenshot_ui_backend::find_window(const hwnd window) const
    {
        const auto found = std::ranges::find(this->windows_, window, &screenshot_window::handle);
        return found == this->windows_.end() ? nullptr : &*found;
    }

    screenshot_window* screenshot_ui_backend::find_mutable(const hwnd window)
    {
        const auto found = std::ranges::find(this->windows_, window, &screenshot_window::handle);
        return found == this->windows_.end() ? nullptr : &*found;
    }

    screenshot_image screenshot_ui_backend::compose() const
    {
        screenshot_image image{.width = this->desktop_width_, .height = this->desktop_height_};
        image.rgba.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u);

        for (size_t i = 0; i < image.rgba.size(); i += 4)
        {
            image.rgba[i + 0] = this->background_[0];
            image.rgba[i + 1] = this->background_[1];
            image.rgba[i + 2] = this->background_[2];
            image.rgba[i + 3] = 0xFF;
        }

        // Creation order is the z-order: the window server appends, so a later window is in front. Nothing
        // here reorders, because an ordering the server did not ask for would be invented.
        for (const auto& window : this->windows_)
        {
            if (!window.visible || window.rgba.empty())
            {
                continue;
            }

            const auto copy_width = std::min(window.width, rect_width(window.rect) > 0 ? rect_width(window.rect) : window.width);
            const auto copy_height = std::min(window.height, rect_height(window.rect) > 0 ? rect_height(window.rect) : window.height);

            for (int y = 0; y < copy_height; ++y)
            {
                const auto target_y = static_cast<int>(window.rect.top) + y;
                if (target_y < 0 || target_y >= image.height)
                {
                    continue;
                }

                for (int x = 0; x < copy_width; ++x)
                {
                    const auto target_x = static_cast<int>(window.rect.left) + x;
                    if (target_x < 0 || target_x >= image.width)
                    {
                        continue;
                    }

                    const auto source = (static_cast<size_t>(y) * static_cast<size_t>(window.width) + static_cast<size_t>(x)) * 4u;
                    const auto target =
                        (static_cast<size_t>(target_y) * static_cast<size_t>(image.width) + static_cast<size_t>(target_x)) * 4u;

                    if (source + 4 > window.rgba.size())
                    {
                        continue;
                    }

                    // An opaque window is copied whatever its alpha says. That is what the flag means, and
                    // it is the difference between a window that has not drawn yet showing as black and
                    // showing as nothing at all.
                    const auto alpha = window.opaque ? uint8_t{0xFF} : window.rgba[source + 3];
                    if (alpha == 0)
                    {
                        continue;
                    }

                    if (alpha == 0xFF)
                    {
                        std::memcpy(image.rgba.data() + target, window.rgba.data() + source, 3);
                        image.rgba[target + 3] = 0xFF;
                        continue;
                    }

                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        const auto over = static_cast<uint32_t>(window.rgba[source + channel]) * alpha;
                        const auto under = static_cast<uint32_t>(image.rgba[target + channel]) * (255u - alpha);
                        image.rgba[target + channel] = static_cast<uint8_t>((over + under) / 255u);
                    }
                }
            }
        }

        return image;
    }

    bool screenshot_ui_backend::write(const std::filesystem::path& path) const
    {
        const auto image = this->compose();
        return write_png_file(path, static_cast<uint32_t>(image.width), static_cast<uint32_t>(image.height),
                              static_cast<uint32_t>(image.width) * 4u, image.rgba);
    }

    void screenshot_ui_backend::post_event(const ui_event& event)
    {
        this->pending_.push_back(event);
    }

    std::unique_ptr<ui_backend> create_screenshot_ui_backend()
    {
        return std::make_unique<screenshot_ui_backend>();
    }
}
