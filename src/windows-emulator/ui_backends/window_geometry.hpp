#pragma once

#include <platform/ui_backend.hpp>

#include <algorithm>
#include <utility>

namespace sogen
{
    // The host surface covers only the client area, while emulated screen coordinates include the guest frame.
    inline std::pair<float, float> screen_point_in_window(const ui_window_desc& desc, const int32_t screen_x, const int32_t screen_y)
    {
        const auto outer_width = std::max<int32_t>(0, desc.rect.right - desc.rect.left);
        const auto outer_height = std::max<int32_t>(0, desc.rect.bottom - desc.rect.top);
        const auto client_x_offset = std::min<int32_t>(outer_width, desc.client_insets.left);
        const auto client_y_offset = std::min<int32_t>(outer_height, desc.client_insets.top);

        return {static_cast<float>(screen_x - desc.rect.left - client_x_offset),
                static_cast<float>(screen_y - desc.rect.top - client_y_offset)};
    }

    inline void move_window_origin(RECT& rect, const int32_t x, const int32_t y)
    {
        rect.right += x - rect.left;
        rect.bottom += y - rect.top;
        rect.left = x;
        rect.top = y;
    }
}
