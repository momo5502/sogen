#include <gtest/gtest.h>

#include <devices/vulkan_bc_emulation.hpp>

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_bc1_rgba_srgb = 134;
        constexpr uint32_t k_bc3_unorm = 137;
        constexpr uint32_t k_bc5_snorm = 142;
        constexpr uint32_t k_bc7_unorm = 145;

        bc_emulated_format format(const uint32_t vk_format)
        {
            return get_bc_emulated_format(vk_format).value();
        }
    }

    TEST(VulkanBcEmulationTest, FormatsMapToTheUncompressedFormatWithTheSameChannels)
    {
        EXPECT_EQ(43u, format(k_bc1_rgba_srgb).substitute); // VK_FORMAT_R8G8B8A8_SRGB
        EXPECT_EQ(37u, format(k_bc3_unorm).substitute);     // VK_FORMAT_R8G8B8A8_UNORM
        EXPECT_EQ(17u, format(k_bc5_snorm).substitute);     // VK_FORMAT_R8G8_SNORM
        EXPECT_EQ(8u, format(k_bc1_rgba_srgb).block_bytes);
        EXPECT_EQ(16u, format(k_bc3_unorm).block_bytes);
        EXPECT_FALSE(get_bc_emulated_format(k_bc7_unorm).has_value());
    }

    // A mip level's blocks cover its extent rounded up to whole 4x4 blocks, down to a single block.
    TEST(VulkanBcEmulationTest, ShadowHoldsEveryMipAndLayerInWholeBlocks)
    {
        const bc_shadow_layout layout(format(k_bc3_unorm), 10, 6, 1, 3, 2);

        EXPECT_EQ(3u, layout.blocks_x(0)); // 10 texels
        EXPECT_EQ(2u, layout.blocks_y(0)); // 6 texels
        EXPECT_EQ(2u, layout.blocks_x(1)); // 5 texels
        EXPECT_EQ(1u, layout.blocks_y(1)); // 3 texels
        EXPECT_EQ(1u, layout.blocks_x(2)); // 2 texels

        const uint64_t mip0 = 3 * 2 * 16;
        const uint64_t mip1 = 2 * 1 * 16;
        const uint64_t mip2 = 1 * 1 * 16;
        EXPECT_EQ((mip0 + mip1 + mip2) * 2, layout.size());
        EXPECT_EQ(mip0, layout.offset(0, 1, 0, 0, 0));
        EXPECT_EQ(mip0 * 2, layout.offset(1, 0, 0, 0, 0));
        EXPECT_EQ((mip0 * 2) + mip1 + 16, layout.offset(1, 1, 0, 0, 1));
    }

    TEST(VulkanBcEmulationTest, CopyOfASubRegionCopiesEachRowOfBlocks)
    {
        const auto bc1 = format(k_bc1_rgba_srgb);
        const bc_shadow_layout layout(bc1, 16, 16, 1, 1, 1);

        // An 8x8 texel region at (4, 8) from a buffer whose rows are 12 texels (3 blocks) long.
        const auto plan = plan_bc_copy(layout, bc1, {.offset = 64, .row_length = 12, .image_height = 0},
                                       {.mip = 0, .base_layer = 0, .layer_count = 1, .x = 4, .y = 8, .width = 8, .height = 8, .depth = 1});
        ASSERT_TRUE(plan.has_value());

        EXPECT_EQ(2u, plan->blocks_x);
        EXPECT_EQ(2u, plan->blocks_y);
        EXPECT_EQ(1u, plan->slices);
        EXPECT_EQ(32u, plan->shadow_row_bytes);
        ASSERT_EQ(2u, plan->rows.size());
        EXPECT_EQ(64u, plan->rows[0].buffer_offset);
        EXPECT_EQ((2 * 32) + 8, plan->rows[0].shadow_offset);
        EXPECT_EQ(16u, plan->rows[0].size);
        EXPECT_EQ(64u + 24, plan->rows[1].buffer_offset);
        EXPECT_EQ((3 * 32) + 8, plan->rows[1].shadow_offset);
        EXPECT_EQ(plan->rows[0].shadow_offset, plan->shadow_offset);
    }

    // Partial blocks are only allowed where the region ends at the edge of the mip level.
    TEST(VulkanBcEmulationTest, CopyOfAWholeLevelArrayIsOneCopyPerLayer)
    {
        const auto bc3 = format(k_bc3_unorm);
        const bc_shadow_layout layout(bc3, 6, 6, 1, 1, 3);

        const auto plan = plan_bc_copy(layout, bc3, {.offset = 0, .row_length = 0, .image_height = 0},
                                       {.mip = 0, .base_layer = 1, .layer_count = 2, .x = 0, .y = 0, .width = 6, .height = 6, .depth = 1});
        ASSERT_TRUE(plan.has_value());

        EXPECT_EQ(2u, plan->slices);
        ASSERT_EQ(2u, plan->rows.size());
        const uint64_t layer_bytes = 2 * 2 * 16;
        EXPECT_EQ(layer_bytes, plan->rows[0].shadow_offset);
        EXPECT_EQ(layer_bytes, plan->rows[0].size);
        EXPECT_EQ(layer_bytes, plan->rows[1].buffer_offset);
        EXPECT_EQ(layer_bytes * 2, plan->rows[1].shadow_offset);
    }

    TEST(VulkanBcEmulationTest, CopiesOutsideTheImageOrOffBlockBoundariesAreRefused)
    {
        const auto bc1 = format(k_bc1_rgba_srgb);
        const bc_shadow_layout layout(bc1, 8, 8, 1, 2, 1);

        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 0, .layer_count = 1, .x = 2, .y = 0, .width = 4, .height = 4, .depth = 1}));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 0, .layer_count = 1, .x = 4, .y = 0, .width = 8, .height = 4, .depth = 1}));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 2, .layer_count = 1, .width = 1, .height = 1, .depth = 1}));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 0, .base_layer = 1, .layer_count = 1, .width = 4, .height = 4, .depth = 1}));
    }

    // vkCmdCopyBufferToImage requires block-aligned offsets, whole blocks except where the region ends at the edge of the
    // mip level, and buffer offsets, row lengths and image heights in whole blocks no smaller than the region.
    TEST(VulkanBcEmulationTest, CopiesFollowTheRulesForBlockCompressedImages)
    {
        const auto bc1 = format(k_bc1_rgba_srgb);
        const bc_shadow_layout layout(bc1, 10, 10, 1, 1, 1);
        const auto region = [](const int32_t x, const uint32_t width) {
            return bc_image_region{.mip = 0, .layer_count = 1, .x = x, .y = 0, .width = width, .height = 4, .depth = 1};
        };

        EXPECT_TRUE(plan_bc_copy(layout, bc1, {}, region(8, 2)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, region(0, 6)));

        EXPECT_TRUE(plan_bc_copy(layout, bc1, {.offset = 8, .row_length = 8, .image_height = 4}, region(0, 8)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {.offset = 4}, region(0, 8)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {.row_length = 4}, region(0, 8)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {.row_length = 10}, region(0, 8)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {.image_height = 2}, region(0, 8)));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {.image_height = 6}, region(0, 8)));
    }

    TEST(VulkanBcEmulationTest, RemainingArrayLayersCoverTheLayersFromTheBaseOn)
    {
        const auto bc3 = format(k_bc3_unorm);
        const bc_shadow_layout layout(bc3, 4, 4, 1, 1, 3);
        constexpr uint32_t remaining_array_layers = ~0u;

        const auto from_second = plan_bc_copy(
            layout, bc3, {}, {.mip = 0, .base_layer = 1, .layer_count = remaining_array_layers, .width = 4, .height = 4, .depth = 1});
        ASSERT_TRUE(from_second.has_value());
        EXPECT_EQ(2u, from_second->slices);

        const auto all =
            plan_bc_copy(layout, bc3, {}, {.mip = 0, .layer_count = remaining_array_layers, .width = 4, .height = 4, .depth = 1});
        ASSERT_TRUE(all.has_value());
        EXPECT_EQ(3u, all->slices);
    }

    TEST(VulkanBcEmulationTest, RegionsWhoseEndWrapsAroundAreRefused)
    {
        const auto bc1 = format(k_bc1_rgba_srgb);
        const bc_shadow_layout layout(bc1, 16, 16, 1, 1, 3);

        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 0, .layer_count = 1, .x = 8, .width = 0xFFFFFFFC, .height = 4, .depth = 1}));
        EXPECT_FALSE(plan_bc_copy(layout, bc1, {}, {.mip = 0, .layer_count = 1, .y = 8, .width = 4, .height = 0xFFFFFFFC, .depth = 1}));
        EXPECT_FALSE(
            plan_bc_copy(layout, bc1, {}, {.mip = 0, .base_layer = 2, .layer_count = 0xFFFFFFFE, .width = 4, .height = 4, .depth = 1}));
    }
}
