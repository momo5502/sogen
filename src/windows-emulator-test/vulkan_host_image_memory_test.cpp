#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>

#include <array>

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_r8_unorm = 9;
        constexpr uint32_t k_r8g8b8a8_unorm = 37;
        constexpr uint32_t k_r16g16b16a16_sfloat = 97;
        constexpr uint32_t k_d24_unorm_s8_uint = 129;
        constexpr uint32_t k_d32_sfloat_s8_uint = 130;
        constexpr uint32_t k_bc7_unorm = 145;

        constexpr uint32_t k_usage_transfer_dst = 0x2;
        constexpr uint32_t k_usage_sampled = 0x4;
        constexpr uint32_t k_usage_storage = 0x8;
        constexpr uint32_t k_usage_color_attachment = 0x10;
        constexpr uint32_t k_usage_depth_stencil_attachment = 0x20;

        constexpr uint32_t k_create_cube_compatible = 0x10;
        constexpr uint32_t k_tiling_optimal = 0;
        constexpr uint32_t k_image_2d = 1;
        constexpr uint32_t k_image_3d = 2;
    }

    // vkGetDeviceImageMemoryRequirements must answer what creating the image and querying it answers, including the
    // transfer usage and host-import handle type the bridge adds to every guest image.
    TEST(VulkanHostTest, DeviceImageMemoryRequirementsMatchACreatedImage)
    {
        vulkan_host host{};
        host.load(nullptr);
        if (!host.available())
        {
            GTEST_SKIP() << "No host Vulkan driver";
        }

        // A Vulkan loader without an installed driver fails here with VK_ERROR_INCOMPATIBLE_DRIVER.
        uint64_t instance = 0;
        if (const auto result = host.create_instance({}, instance); result != 0)
        {
            GTEST_SKIP() << "No host Vulkan driver (vkCreateInstance returned " << result << ")";
        }

        std::array<uint64_t, 1> physical_devices{};
        uint32_t physical_device_count = 0;
        host.enumerate_physical_devices(instance, physical_devices, physical_device_count);
        if (physical_device_count == 0)
        {
            host.destroy_instance(instance);
            GTEST_SKIP() << "No host Vulkan device";
        }

        uint64_t device = 0;
        ASSERT_EQ(host.create_device(physical_devices[0], nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, false, device), 0);

        struct image_parameters
        {
            uint32_t format, width, height, usage, tiling, samples, image_type, depth, mip_levels, array_layers, flags;
        };

        constexpr std::array images{
            image_parameters{k_r8g8b8a8_unorm, 1024, 1024, k_usage_sampled | k_usage_transfer_dst, k_tiling_optimal, 1, k_image_2d, 1, 11,
                             1, 0},
            image_parameters{k_d24_unorm_s8_uint, 1280, 720, k_usage_depth_stencil_attachment, k_tiling_optimal, 1, k_image_2d, 1, 1, 1, 0},
            image_parameters{k_d32_sfloat_s8_uint, 1280, 720, k_usage_depth_stencil_attachment, k_tiling_optimal, 1, k_image_2d, 1, 1, 1,
                             0},
            image_parameters{k_r16g16b16a16_sfloat, 1280, 720, k_usage_color_attachment | k_usage_sampled, k_tiling_optimal, 4, k_image_2d,
                             1, 1, 1, 0},
            image_parameters{k_bc7_unorm, 256, 256, k_usage_sampled | k_usage_transfer_dst, k_tiling_optimal, 1, k_image_2d, 1, 9, 6,
                             k_create_cube_compatible},
            image_parameters{k_r8_unorm, 64, 64, k_usage_storage, k_tiling_optimal, 1, k_image_3d, 64, 1, 1, 0},
        };

        size_t compared = 0;
        for (const auto& i : images)
        {
            SCOPED_TRACE(testing::Message() << "format " << i.format);

            // Only images the device supports are valid to create or query: AMD has no D24 depth format, for example.
            vulkan_host::image_format_properties properties{};
            if (host.get_physical_device_image_format_properties(physical_devices[0], i.format, i.image_type, i.tiling, i.usage, i.flags,
                                                                 properties) != 0 ||
                (properties.sample_counts & i.samples) == 0)
            {
                continue;
            }
            ++compared;

            uint64_t image = 0;
            ASSERT_EQ(host.create_image(device, i.format, i.width, i.height, i.usage, i.tiling, i.samples, i.image_type, i.depth,
                                        i.mip_levels, i.array_layers, i.flags, image),
                      0);

            uint64_t size = 0;
            uint64_t alignment = 0;
            uint32_t memory_type_bits = 0;
            ASSERT_EQ(host.get_image_memory_requirements(device, image, size, alignment, memory_type_bits), 0);
            host.destroy_image(device, image);

            uint64_t queried_size = 0;
            uint64_t queried_alignment = 0;
            uint32_t queried_memory_type_bits = 0;
            ASSERT_EQ(host.get_device_image_memory_requirements(device, i.format, i.width, i.height, i.usage, i.tiling, i.samples,
                                                                i.image_type, i.depth, i.mip_levels, i.array_layers, i.flags, queried_size,
                                                                queried_alignment, queried_memory_type_bits),
                      0);

            EXPECT_NE(size, 0u);
            EXPECT_EQ(queried_size, size);
            EXPECT_EQ(queried_alignment, alignment);
            EXPECT_EQ(queried_memory_type_bits, memory_type_bits);
        }

        // Which shapes a device supports varies (BC7 and an R8_UNORM storage image are optional), so only one is required.
        EXPECT_GE(compared, 1u);

        host.destroy_device(device);
        host.destroy_instance(instance);
    }
}
