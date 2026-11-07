#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <platform/platform.hpp>

namespace sogen::gdi
{
    // The pixels a drawing call may change, as non-overlapping rectangles in surface coordinates.
    class clip_region
    {
      public:
        clip_region() = default;

        explicit clip_region(const RECT& area)
        {
            if (!is_empty(area))
            {
                this->rects_.push_back(area);
            }
        }

        static clip_region whole(const uint32_t width, const uint32_t height)
        {
            return clip_region{RECT{.left = 0, .top = 0, .right = static_cast<LONG>(width), .bottom = static_cast<LONG>(height)}};
        }

        void intersect(const RECT& area)
        {
            for (auto& rect : this->rects_)
            {
                rect = intersection(rect, area);
            }
            std::erase_if(this->rects_, is_empty);
        }

        void subtract(const RECT& area)
        {
            const auto original_count = this->rects_.size();
            for (size_t i = 0; i < original_count; ++i)
            {
                const RECT rect = this->rects_[i];
                const RECT overlap = intersection(rect, area);
                if (is_empty(overlap))
                {
                    continue;
                }

                this->rects_[i] = {.left = rect.left, .top = rect.top, .right = rect.right, .bottom = overlap.top};
                append_if_not_empty(this->rects_, {.left = rect.left, .top = overlap.bottom, .right = rect.right, .bottom = rect.bottom});
                append_if_not_empty(this->rects_, {.left = rect.left, .top = overlap.top, .right = overlap.left, .bottom = overlap.bottom});
                append_if_not_empty(this->rects_,
                                    {.left = overlap.right, .top = overlap.top, .right = rect.right, .bottom = overlap.bottom});
            }
            std::erase_if(this->rects_, is_empty);
        }

        bool contains(const int64_t x, const int64_t y) const
        {
            return std::ranges::any_of(
                this->rects_, [&](const RECT& rect) { return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom; });
        }

        // Calls visit(x_begin, x_end, y) once per visible run of each row of `area`.
        template <typename Visitor>
        void for_each_span(const RECT& area, Visitor&& visit) const
        {
            for (const auto& rect : this->rects_)
            {
                const RECT overlap = intersection(rect, area);
                for (auto y = overlap.top; y < overlap.bottom; ++y)
                {
                    if (overlap.left < overlap.right)
                    {
                        visit(overlap.left, overlap.right, y);
                    }
                }
            }
        }

        const std::vector<RECT>& rects() const
        {
            return this->rects_;
        }

      private:
        std::vector<RECT> rects_{};

        static bool is_empty(const RECT& rect)
        {
            return rect.left >= rect.right || rect.top >= rect.bottom;
        }

        static RECT intersection(const RECT& a, const RECT& b)
        {
            return {.left = std::max(a.left, b.left),
                    .top = std::max(a.top, b.top),
                    .right = std::min(a.right, b.right),
                    .bottom = std::min(a.bottom, b.bottom)};
        }

        static void append_if_not_empty(std::vector<RECT>& rects, const RECT& rect)
        {
            if (!is_empty(rect))
            {
                rects.push_back(rect);
            }
        }
    };
}
