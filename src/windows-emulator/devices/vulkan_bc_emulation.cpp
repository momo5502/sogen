#include "vulkan_bc_emulation.hpp"

#include <algorithm>

namespace sogen
{
    namespace
    {
        // VkFormat values from the Vulkan headers, which this file does not include.
        constexpr uint32_t k_r8_unorm = 9;
        constexpr uint32_t k_r8_snorm = 10;
        constexpr uint32_t k_r8g8_unorm = 16;
        constexpr uint32_t k_r8g8_snorm = 17;
        constexpr uint32_t k_r8g8b8a8_unorm = 37;
        constexpr uint32_t k_r8g8b8a8_srgb = 43;

        constexpr uint32_t k_bc1_rgb_unorm = 131;
        constexpr uint32_t k_bc1_rgb_srgb = 132;
        constexpr uint32_t k_bc1_rgba_unorm = 133;
        constexpr uint32_t k_bc1_rgba_srgb = 134;
        constexpr uint32_t k_bc2_unorm = 135;
        constexpr uint32_t k_bc2_srgb = 136;
        constexpr uint32_t k_bc3_unorm = 137;
        constexpr uint32_t k_bc3_srgb = 138;
        constexpr uint32_t k_bc4_unorm = 139;
        constexpr uint32_t k_bc4_snorm = 140;
        constexpr uint32_t k_bc5_unorm = 141;
        constexpr uint32_t k_bc5_snorm = 142;

        constexpr uint32_t k_block = 4;

        uint32_t blocks(const uint32_t texels)
        {
            return (texels + k_block - 1) / k_block;
        }

        uint32_t mip_extent(const uint32_t extent, const uint32_t mip)
        {
            return std::max(1u, extent >> mip);
        }
    }

    std::optional<bc_emulated_format> get_bc_emulated_format(const uint32_t format)
    {
        switch (format)
        {
        case k_bc1_rgb_unorm:
            return bc_emulated_format{k_r8g8b8a8_unorm, bc_decode_kind::bc1_rgb, 8, 4};
        case k_bc1_rgb_srgb:
            return bc_emulated_format{k_r8g8b8a8_srgb, bc_decode_kind::bc1_rgb, 8, 4};
        case k_bc1_rgba_unorm:
            return bc_emulated_format{k_r8g8b8a8_unorm, bc_decode_kind::bc1_rgba, 8, 4};
        case k_bc1_rgba_srgb:
            return bc_emulated_format{k_r8g8b8a8_srgb, bc_decode_kind::bc1_rgba, 8, 4};
        case k_bc2_unorm:
            return bc_emulated_format{k_r8g8b8a8_unorm, bc_decode_kind::bc2, 16, 4};
        case k_bc2_srgb:
            return bc_emulated_format{k_r8g8b8a8_srgb, bc_decode_kind::bc2, 16, 4};
        case k_bc3_unorm:
            return bc_emulated_format{k_r8g8b8a8_unorm, bc_decode_kind::bc3, 16, 4};
        case k_bc3_srgb:
            return bc_emulated_format{k_r8g8b8a8_srgb, bc_decode_kind::bc3, 16, 4};
        case k_bc4_unorm:
            return bc_emulated_format{k_r8_unorm, bc_decode_kind::bc4_unorm, 8, 1};
        case k_bc4_snorm:
            return bc_emulated_format{k_r8_snorm, bc_decode_kind::bc4_snorm, 8, 1};
        case k_bc5_unorm:
            return bc_emulated_format{k_r8g8_unorm, bc_decode_kind::bc5_unorm, 16, 2};
        case k_bc5_snorm:
            return bc_emulated_format{k_r8g8_snorm, bc_decode_kind::bc5_snorm, 16, 2};
        default:
            return std::nullopt;
        }
    }

    bc_shadow_layout::bc_shadow_layout(const bc_emulated_format& format, const uint32_t width, const uint32_t height, const uint32_t depth,
                                       const uint32_t mip_levels, const uint32_t array_layers)
        : block_bytes_(format.block_bytes)
    {
        const auto levels = std::max(1u, mip_levels);
        const auto layers = std::max(1u, array_layers);
        this->array_layers_ = layers;
        this->mips_.reserve(levels);
        for (uint32_t mip = 0; mip < levels; ++mip)
        {
            mip_layout level{};
            level.blocks_x = blocks(mip_extent(width, mip));
            level.blocks_y = blocks(mip_extent(height, mip));
            level.depth = mip_extent(std::max(1u, depth), mip);
            level.offset = this->size_;
            level.layer_bytes = static_cast<uint64_t>(level.blocks_x) * level.blocks_y * level.depth * this->block_bytes_;
            this->size_ += level.layer_bytes * layers;
            this->mips_.push_back(level);
        }
    }

    uint32_t bc_shadow_layout::blocks_x(const uint32_t mip) const
    {
        return this->mips_.at(mip).blocks_x;
    }

    uint32_t bc_shadow_layout::blocks_y(const uint32_t mip) const
    {
        return this->mips_.at(mip).blocks_y;
    }

    uint32_t bc_shadow_layout::depth(const uint32_t mip) const
    {
        return this->mips_.at(mip).depth;
    }

    uint64_t bc_shadow_layout::row_bytes(const uint32_t mip) const
    {
        return static_cast<uint64_t>(this->mips_.at(mip).blocks_x) * this->block_bytes_;
    }

    uint64_t bc_shadow_layout::slice_bytes(const uint32_t mip) const
    {
        return this->row_bytes(mip) * this->mips_.at(mip).blocks_y;
    }

    uint64_t bc_shadow_layout::subresource_bytes(const uint32_t mip) const
    {
        return this->mips_.at(mip).layer_bytes;
    }

    uint64_t bc_shadow_layout::offset(const uint32_t mip, const uint32_t layer, const uint32_t z, const uint32_t block_row,
                                      const uint32_t block_column) const
    {
        const auto& level = this->mips_.at(mip);
        return level.offset + (layer * level.layer_bytes) + (z * this->slice_bytes(mip)) + (block_row * this->row_bytes(mip)) +
               (static_cast<uint64_t>(block_column) * this->block_bytes_);
    }

    std::optional<bc_copy_plan> plan_bc_copy(const bc_shadow_layout& layout, const bc_emulated_format& format,
                                             const bc_buffer_region& buffer, const bc_image_region& image)
    {
        const auto layers = std::max(1u, image.layer_count);
        if (image.mip >= layout.mip_levels() || image.base_layer + layers > layout.array_layers())
        {
            return std::nullopt;
        }

        if (image.x < 0 || image.y < 0 || image.z < 0 || image.x % k_block != 0 || image.y % k_block != 0 || image.width == 0 ||
            image.height == 0)
        {
            return std::nullopt;
        }

        const auto block_x = static_cast<uint32_t>(image.x) / k_block;
        const auto block_y = static_cast<uint32_t>(image.y) / k_block;
        const auto blocks_wide = blocks(image.width);
        const auto blocks_high = blocks(image.height);
        const auto depth = std::max(1u, image.depth);
        if (block_x + blocks_wide > layout.blocks_x(image.mip) || block_y + blocks_high > layout.blocks_y(image.mip) ||
            static_cast<uint32_t>(image.z) + depth > layout.depth(image.mip) || (depth > 1 && layers > 1))
        {
            return std::nullopt;
        }

        const uint64_t buffer_row_bytes =
            static_cast<uint64_t>(blocks(buffer.row_length != 0 ? buffer.row_length : image.width)) * format.block_bytes;
        const uint64_t buffer_slice_bytes = buffer_row_bytes * blocks(buffer.image_height != 0 ? buffer.image_height : image.height);
        const uint64_t row_bytes = static_cast<uint64_t>(blocks_wide) * format.block_bytes;

        bc_copy_plan plan{};
        plan.blocks_x = blocks_wide;
        plan.blocks_y = blocks_high;
        plan.slices = layers * depth;
        plan.shadow_row_bytes = layout.row_bytes(image.mip);
        plan.shadow_slice_bytes = layout.slice_bytes(image.mip);
        plan.shadow_offset = layout.offset(image.mip, image.base_layer, static_cast<uint32_t>(image.z), block_y, block_x);

        const bool whole_rows = row_bytes == layout.row_bytes(image.mip) && row_bytes == buffer_row_bytes;
        for (uint32_t slice = 0; slice < plan.slices; ++slice)
        {
            const uint32_t layer = depth > 1 ? image.base_layer : image.base_layer + slice;
            const uint32_t z = static_cast<uint32_t>(image.z) + (depth > 1 ? slice : 0);
            const uint64_t buffer_slice = buffer.offset + (slice * buffer_slice_bytes);
            if (whole_rows)
            {
                plan.rows.push_back({.buffer_offset = buffer_slice,
                                     .shadow_offset = layout.offset(image.mip, layer, z, block_y, 0),
                                     .size = row_bytes * blocks_high});
                continue;
            }

            for (uint32_t row = 0; row < blocks_high; ++row)
            {
                plan.rows.push_back({.buffer_offset = buffer_slice + (row * buffer_row_bytes),
                                     .shadow_offset = layout.offset(image.mip, layer, z, block_y + row, block_x),
                                     .size = row_bytes});
            }
        }

        return plan;
    }
}
