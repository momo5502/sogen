#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
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
            this->append_if_not_empty(area);
        }

        static clip_region whole(const uint32_t width, const uint32_t height)
        {
            return clip_region{RECT{.left = 0, .top = 0, .right = static_cast<LONG>(width), .bottom = static_cast<LONG>(height)}};
        }

        void intersect(const RECT& area)
        {
            for (auto& rect : this->storage())
            {
                rect = intersection(rect, area);
            }
            this->remove_empty();
        }

        void subtract(const RECT& area)
        {
            const auto original_count = this->count_;
            for (size_t i = 0; i < original_count; ++i)
            {
                const RECT rect = this->storage()[i];
                const RECT overlap = intersection(rect, area);
                if (is_empty(overlap))
                {
                    continue;
                }

                this->storage()[i] = {.left = rect.left, .top = rect.top, .right = rect.right, .bottom = overlap.top};
                this->append_if_not_empty({.left = rect.left, .top = overlap.bottom, .right = rect.right, .bottom = rect.bottom});
                this->append_if_not_empty({.left = rect.left, .top = overlap.top, .right = overlap.left, .bottom = overlap.bottom});
                this->append_if_not_empty({.left = overlap.right, .top = overlap.top, .right = rect.right, .bottom = overlap.bottom});
            }
            this->remove_empty();
        }

        bool contains(const int64_t x, const int64_t y) const
        {
            return std::ranges::any_of(
                this->rects(), [&](const RECT& rect) { return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom; });
        }

        // Calls visit(x_begin, x_end, y) once per visible run of each row of `area`.
        template <typename Visitor>
        void for_each_span(const RECT& area, Visitor&& visit) const
        {
            for (const auto& rect : this->rects())
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

        std::span<const RECT> rects() const
        {
            return this->spilled_.empty() ? std::span<const RECT>{this->inline_.data(), this->count_}
                                          : std::span<const RECT>{this->spilled_};
        }

      private:
        // A window's visible region is rarely more than a few rectangles, so it is resolved on every draw without touching the
        // heap; only a region fragmented beyond this moves to the vector.
        static constexpr size_t inline_capacity = 16;

        std::array<RECT, inline_capacity> inline_{};
        std::vector<RECT> spilled_{};
        size_t count_{};

        std::span<RECT> storage()
        {
            return this->spilled_.empty() ? std::span<RECT>{this->inline_.data(), this->count_} : std::span<RECT>{this->spilled_};
        }

        void append_if_not_empty(const RECT& rect)
        {
            if (is_empty(rect))
            {
                return;
            }

            if (this->spilled_.empty() && this->count_ < inline_capacity)
            {
                this->inline_[this->count_++] = rect;
                return;
            }

            if (this->spilled_.empty())
            {
                this->spilled_.assign(this->inline_.begin(), this->inline_.begin() + static_cast<std::ptrdiff_t>(this->count_));
            }
            this->spilled_.push_back(rect);
            this->count_ = this->spilled_.size();
        }

        void remove_empty()
        {
            const auto rects = this->storage();
            const auto kept = static_cast<size_t>(std::ranges::remove_if(rects, is_empty).begin() - rects.begin());
            if (!this->spilled_.empty())
            {
                this->spilled_.resize(kept);
            }
            this->count_ = kept;
        }

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
    };
}
