#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace sogen
{
    // Matches the KIND_ constants of snapshot_shaders/bc_decode.comp.glsl.
    enum class bc_decode_kind : uint32_t
    {
        bc1_rgb,
        bc1_rgba,
        bc2,
        bc3,
        bc4_unorm,
        bc4_snorm,
        bc5_unorm,
        bc5_snorm,
    };

    // A BC format a device without textureCompressionBC backs with an uncompressed format of the same channels.
    struct bc_emulated_format
    {
        uint32_t substitute{};
        bc_decode_kind kind{};
        uint32_t block_bytes{};
        uint32_t texel_bytes{};
    };

    std::optional<bc_emulated_format> get_bc_emulated_format(uint32_t format);

    // The compressed contents of an emulated image, kept because the backing image cannot give them back: per mip
    // level, then per array layer, then per depth slice, rows of blocks without padding.
    class bc_shadow_layout
    {
      public:
        bc_shadow_layout(const bc_emulated_format& format, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_levels,
                         uint32_t array_layers);

        uint64_t size() const
        {
            return this->size_;
        }

        uint32_t mip_levels() const
        {
            return static_cast<uint32_t>(this->mips_.size());
        }

        uint32_t array_layers() const
        {
            return this->array_layers_;
        }

        uint32_t width(uint32_t mip) const;
        uint32_t height(uint32_t mip) const;
        uint32_t blocks_x(uint32_t mip) const;
        uint32_t blocks_y(uint32_t mip) const;
        uint32_t depth(uint32_t mip) const;
        uint64_t row_bytes(uint32_t mip) const;
        uint64_t slice_bytes(uint32_t mip) const;
        uint64_t subresource_bytes(uint32_t mip) const;
        uint64_t offset(uint32_t mip, uint32_t layer, uint32_t z, uint32_t block_row, uint32_t block_column) const;

      private:
        struct mip_layout
        {
            uint32_t width{};
            uint32_t height{};
            uint32_t blocks_x{};
            uint32_t blocks_y{};
            uint32_t depth{};
            uint64_t offset{};
            uint64_t layer_bytes{};
        };

        uint32_t block_bytes_{};
        uint32_t array_layers_{};
        std::vector<mip_layout> mips_{};
        uint64_t size_{};
    };

    struct bc_buffer_region
    {
        uint64_t offset{};
        uint32_t row_length{};
        uint32_t image_height{};
    };

    struct bc_image_region
    {
        uint32_t mip{};
        uint32_t base_layer{};
        uint32_t layer_count{};
        int32_t x{};
        int32_t y{};
        int32_t z{};
        uint32_t width{};
        uint32_t height{};
        uint32_t depth{};
    };

    struct bc_byte_copy
    {
        uint64_t buffer_offset{};
        uint64_t shadow_offset{};
        uint64_t size{};
    };

    // What one buffer<->image copy of an emulated image touches: the rows of blocks the compressed data occupies in the
    // guest buffer and in the shadow, and the block range the decode covers. Copies between the guest buffer and the
    // shadow are one row of blocks each unless whole slices line up.
    struct bc_copy_plan
    {
        std::vector<bc_byte_copy> rows{};
        uint64_t shadow_offset{};
        uint32_t blocks_x{};
        uint32_t blocks_y{};
        uint32_t slices{};
        uint64_t shadow_row_bytes{};
        uint64_t shadow_slice_bytes{};
    };

    // Nothing for regions Vulkan does not allow for block-compressed images: offsets that are not block aligned, partial
    // blocks other than at the edge of the mip level, buffer offsets, rows or images that are not whole blocks or are smaller
    // than the region, and regions that leave the image.
    std::optional<bc_copy_plan> plan_bc_copy(const bc_shadow_layout& layout, const bc_emulated_format& format,
                                             const bc_buffer_region& buffer, const bc_image_region& image);
}
