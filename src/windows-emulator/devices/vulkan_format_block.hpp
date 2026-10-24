#pragma once

#include <cstdint>
#include <optional>

namespace sogen
{
    struct vulkan_format_block
    {
        uint32_t size{};
        uint32_t width{};
        uint32_t height{};
    };

    // The texel block a buffer copy of one aspect of an image in this format uses. Nothing for formats with several planes.
    std::optional<vulkan_format_block> get_vulkan_copy_block(uint32_t format, uint32_t aspect_mask);

    // The aspects images in this format have: color, or depth and/or stencil.
    uint32_t get_vulkan_format_aspects(uint32_t format);
}
