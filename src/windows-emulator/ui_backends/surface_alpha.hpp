#pragma once

#include <platform/ui_backend.hpp>

#include <cstdint>
#include <span>

namespace sogen
{
    // What UpdateLayeredWindow's BLENDFUNCTION asks of the source bitmap, applied before upload because presentation
    // can only blend pixels that already carry their own alpha.
    //
    // With AC_SRC_ALPHA the source is premultiplied and SourceConstantAlpha scales all four channels. Without it
    // Windows ignores the source alpha entirely and shows the window at SourceConstantAlpha, so the alpha becomes
    // that value and the colours are premultiplied by it. GDI leaves most bitmaps at alpha 0, which is why passing
    // them through unchanged makes such a window disappear.
    inline void apply_layered_blend(const std::span<uint32_t> pixels, const bool source_has_alpha, const uint8_t constant_alpha)
    {
        const auto scale = [constant_alpha](const uint32_t channel) {
            return static_cast<uint32_t>((channel * constant_alpha + 127) / 255);
        };

        for (auto& pixel : pixels)
        {
            const auto blue = pixel & 0xFF;
            const auto green = (pixel >> 8) & 0xFF;
            const auto red = (pixel >> 16) & 0xFF;
            const auto alpha = source_has_alpha ? (pixel >> 24) & 0xFF : 0xFFu;

            pixel = (scale(alpha) << 24) | (scale(red) << 16) | (scale(green) << 8) | scale(blue);
        }
    }
}
