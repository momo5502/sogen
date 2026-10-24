#include "vulkan_format_block.hpp"

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan_format_traits.hpp>

namespace sogen
{
    uint32_t get_vulkan_format_aspects(const uint32_t format)
    {
        switch (static_cast<VkFormat>(format))
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT:
            return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
    }

    std::optional<vulkan_format_block> get_vulkan_copy_block(const uint32_t format, const uint32_t aspect_mask)
    {
        const auto vk_format = static_cast<VkFormat>(format);

        // Buffer copies of a depth or stencil aspect use the aspect's own layout, not the combined format's.
        if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT)
        {
            return vulkan_format_block{.size = 1, .width = 1, .height = 1};
        }

        if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            switch (vk_format)
            {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D16_UNORM_S8_UINT:
                return vulkan_format_block{.size = 2, .width = 1, .height = 1};
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return vulkan_format_block{.size = 4, .width = 1, .height = 1};
            default:
                return std::nullopt;
            }
        }

        const auto traits_format = static_cast<vk::Format>(vk_format);
        if (vk::planeCount(traits_format) != 1)
        {
            return std::nullopt;
        }

        const auto size = vk::blockSize(traits_format);
        if (size == 0)
        {
            return std::nullopt;
        }

        const auto extent = vk::blockExtent(traits_format);
        if (extent[2] != 1)
        {
            return std::nullopt;
        }

        return vulkan_format_block{.size = size, .width = extent[0], .height = extent[1]};
    }
}
