#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>

#include <array>

namespace sogen::test
{
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

        uint64_t instance = 0;
        ASSERT_EQ(host.create_instance({}, instance), 0);

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

        // Formats, usages and flags by their VkFormat, VkImageUsageFlagBits and VkImageCreateFlagBits values.
        constexpr std::array images{
            image_parameters{37, 1024, 1024, 0x4 | 0x2, 0, 1, 1, 1, 11, 1, 0},  // R8G8B8A8_UNORM, SAMPLED | TRANSFER_DST, mips
            image_parameters{129, 1280, 720, 0x20, 0, 1, 1, 1, 1, 1, 0},        // D24_UNORM_S8_UINT, DEPTH_STENCIL_ATTACHMENT
            image_parameters{130, 1280, 720, 0x20, 0, 1, 1, 1, 1, 1, 0},        // D32_SFLOAT_S8_UINT, DEPTH_STENCIL_ATTACHMENT
            image_parameters{97, 1280, 720, 0x10 | 0x4, 0, 4, 1, 1, 1, 1, 0},   // R16G16B16A16_SFLOAT, 4x MSAA attachment
            image_parameters{145, 256, 256, 0x4 | 0x2, 0, 1, 1, 1, 9, 6, 0x10}, // BC7_UNORM_BLOCK cube, CUBE_COMPATIBLE
            image_parameters{9, 64, 64, 0x8, 0, 1, 2, 64, 1, 1, 0},             // R8_UNORM 3D, STORAGE
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

        EXPECT_GE(compared, 4u);

        host.destroy_device(device);
        host.destroy_instance(instance);
    }
}
