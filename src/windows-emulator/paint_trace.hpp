#pragma once

#include <algorithm>
#include <atomic>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>

namespace sogen::paint_trace
{
    struct surface_summary
    {
        bool valid{};
        size_t pixel_count{};
        uint64_t checksum{14695981039346656037ull};
        size_t white_pixels{};
        size_t black_pixels{};
        size_t non_white_pixels{};
        int non_white_left{-1};
        int non_white_top{-1};
        int non_white_right{-1};
        int non_white_bottom{-1};
        uint32_t top_left{};
        uint32_t center{};
        uint32_t bottom_right{};
    };

    inline bool enabled()
    {
        static const bool value = [] {
            const auto* env = std::getenv("SOGEN_PAINT_TRACE");
            return env != nullptr && (std::string_view{env} == "1" || std::string_view{env} == "true");
        }();
        return value;
    }

    inline std::mutex& output_mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline uint64_t next_sequence()
    {
        static std::atomic<uint64_t> sequence{};
        return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    template <typename... Args>
    inline void log(const char* format, Args&&... args)
    {
        if (!enabled())
        {
            return;
        }

        std::array<char, 2048> message{};
        const auto formatted = std::snprintf(message.data(), message.size(), format, std::forward<Args>(args)...);
        const auto length = formatted <= 0 ? size_t{} : std::min(static_cast<size_t>(formatted), static_cast<size_t>(message.size() - 1));

        const std::scoped_lock lock(output_mutex());
        std::fprintf(stderr, "[paint-trace #%06" PRIu64 "] ", next_sequence());
        std::fwrite(message.data(), 1, length, stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    inline surface_summary summarize_bgra8(const void* pixels, const int width, const int height, const int stride)
    {
        surface_summary summary{};
        if (pixels == nullptr || width <= 0 || height <= 0 || stride < 0 ||
            static_cast<size_t>(stride) < static_cast<size_t>(width) * sizeof(uint32_t))
        {
            return summary;
        }

        summary.valid = true;
        summary.pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);

        const auto* data = static_cast<const uint8_t*>(pixels);
        for (int y = 0; y < height; ++y)
        {
            const auto* row = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
            for (int x = 0; x < width; ++x)
            {
                uint32_t pixel{};
                std::memcpy(&pixel, row + static_cast<size_t>(x) * sizeof(pixel), sizeof(pixel));
                summary.checksum = (summary.checksum ^ pixel) * 1099511628211ull;

                const auto rgb = pixel & 0x00FFFFFFu;
                if (rgb == 0x00FFFFFFu)
                {
                    ++summary.white_pixels;
                }
                if (rgb == 0)
                {
                    ++summary.black_pixels;
                }
                if (rgb != 0x00FFFFFFu)
                {
                    ++summary.non_white_pixels;
                    if (summary.non_white_left < 0)
                    {
                        summary.non_white_left = x;
                        summary.non_white_top = y;
                        summary.non_white_right = x;
                        summary.non_white_bottom = y;
                    }
                    else
                    {
                        summary.non_white_left = std::min(summary.non_white_left, x);
                        summary.non_white_top = std::min(summary.non_white_top, y);
                        summary.non_white_right = std::max(summary.non_white_right, x);
                        summary.non_white_bottom = std::max(summary.non_white_bottom, y);
                    }
                }

                if (x == 0 && y == 0)
                {
                    summary.top_left = pixel;
                }
                if (x == width / 2 && y == height / 2)
                {
                    summary.center = pixel;
                }
                if (x == width - 1 && y == height - 1)
                {
                    summary.bottom_right = pixel;
                }
            }
        }

        return summary;
    }

    inline void log_surface(const char* stage, const uint64_t target, const void* pixels, const int width, const int height,
                            const int stride)
    {
        if (!enabled())
        {
            return;
        }

        const auto summary = summarize_bgra8(pixels, width, height, stride);
        if (!summary.valid)
        {
            log("%s target=0x%" PRIx64 " invalid-surface pixels=%p size=%dx%d stride=%d", stage, target, pixels, width, height, stride);
            return;
        }

        log("%s target=0x%" PRIx64 " size=%dx%d stride=%d pixels=%zu hash=%016" PRIx64
            " white=%zu black=%zu nonwhite=%zu bounds=[%d,%d-%d,%d] samples=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32,
            stage, target, width, height, stride, summary.pixel_count, summary.checksum, summary.white_pixels, summary.black_pixels,
            summary.non_white_pixels, summary.non_white_left, summary.non_white_top, summary.non_white_right, summary.non_white_bottom,
            summary.top_left, summary.center, summary.bottom_right);
    }
} // namespace sogen::paint_trace
