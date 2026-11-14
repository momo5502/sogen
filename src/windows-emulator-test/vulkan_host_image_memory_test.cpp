#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

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
    // transfer usage the bridge adds to guest images and the host-import form it may re-create them in.
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

    namespace
    {
        uint32_t first_type(const VkPhysicalDeviceMemoryProperties& memory, const uint32_t type_bits, const bool host_visible)
        {
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            {
                const auto flags = memory.memoryTypes[i].propertyFlags;
                const bool visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
                if ((type_bits & (1u << i)) != 0 && visible == host_visible &&
                    (visible || (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0))
                {
                    return i;
                }
            }
            return UINT32_MAX;
        }

        // Imports host memory for a resource of the given requirements into the first of its host-visible types that takes it.
        uint64_t import_for(vulkan_host& host, const uint64_t device, const VkPhysicalDeviceMemoryProperties& memory, const uint64_t size,
                            const uint32_t type_bits, std::vector<std::byte>& storage)
        {
            const auto alignment = host.host_import_alignment(device);
            const auto import_size = (size + alignment - 1) / alignment * alignment;
            storage.resize(import_size + alignment);
            const auto address = (reinterpret_cast<uintptr_t>(storage.data()) + alignment - 1) / alignment * alignment;
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            {
                uint64_t imported = 0;
                if ((type_bits & (1u << i)) != 0 && host.is_host_visible_memory_type(device, i) &&
                    host.import_host_memory(device, import_size, i, 0, 0, reinterpret_cast<void*>(address), imported) == VK_SUCCESS)
                {
                    return imported;
                }
            }
            return 0;
        }
    }

    // Drivers may offer resources created for host-allocation handles only importable system memory, as AMD does. Images
    // and buffers that imported memory could back must still be able to use device-local memory, and imported memory as
    // well, which the bridge re-creates them for when it is bound.
    TEST(VulkanHostTest, ImportableResourcesBindDeviceLocalAndImportedMemory)
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

        VkPhysicalDeviceMemoryProperties memory{};
        ASSERT_EQ(host.get_physical_device_memory_properties(physical_devices[0], &memory, sizeof(memory)), 0);
        if (host.host_import_alignment(device) == 0 || first_type(memory, ~0u, false) == UINT32_MAX)
        {
            host.destroy_device(device);
            host.destroy_instance(instance);
            GTEST_SKIP() << "No host memory import, or no device-local-only memory";
        }

        std::vector<std::byte> image_storage{};
        std::vector<std::byte> buffer_storage{};

        // R8G8B8A8_UNORM 256x256, SAMPLED | TRANSFER_DST, and buffers for TRANSFER_SRC | TRANSFER_DST | UNIFORM_BUFFER |
        // VERTEX_BUFFER.
        uint64_t local_image = 0;
        uint64_t imported_image = 0;
        uint64_t local_buffer = 0;
        uint64_t imported_buffer = 0;
        ASSERT_EQ(host.create_image(device, 37, 256, 256, 0x4 | 0x2, 0, 1, 1, 1, 1, 1, 0, local_image), 0);
        ASSERT_EQ(host.create_image(device, 37, 256, 256, 0x4 | 0x2, 0, 1, 1, 1, 1, 1, 0, imported_image), 0);
        ASSERT_EQ(host.create_buffer(device, 65536, 0x1 | 0x2 | 0x10 | 0x80, local_buffer), 0);
        ASSERT_EQ(host.create_buffer(device, 65536, 0x1 | 0x2 | 0x10 | 0x80, imported_buffer), 0);

        // A driver may import host allocations for some formats, tilings or usages and not others.
        if (!host.image_takes_host_import(device, imported_image) || !host.buffer_takes_host_import(device, imported_buffer))
        {
            host.destroy_image(device, local_image);
            host.destroy_image(device, imported_image);
            host.destroy_buffer(device, local_buffer);
            host.destroy_buffer(device, imported_buffer);
            host.destroy_device(device);
            host.destroy_instance(instance);
            GTEST_SKIP() << "The device does not import host memory for this image or buffer";
        }

        uint64_t image_size = 0;
        uint64_t image_alignment = 0;
        uint32_t image_types = 0;
        ASSERT_EQ(host.get_image_memory_requirements(device, local_image, image_size, image_alignment, image_types), 0);

        const auto image_local_type = first_type(memory, image_types, false);
        ASSERT_NE(image_local_type, UINT32_MAX) << "memory types 0x" << std::hex << image_types;
        uint64_t image_local_memory = 0;
        ASSERT_EQ(host.allocate_memory(device, image_size, image_local_type, 0, 0, image_local_memory), 0);
        EXPECT_EQ(host.bind_image_memory(device, local_image, image_local_memory, 0), 0);

        const auto image_imported_memory = import_for(host, device, memory, image_size, image_types, image_storage);
        ASSERT_NE(image_imported_memory, 0u);
        EXPECT_EQ(host.bind_image_memory(device, imported_image, image_imported_memory, 0), 0);

        uint64_t buffer_size = 0;
        uint64_t buffer_alignment = 0;
        uint32_t buffer_types = 0;
        ASSERT_EQ(host.get_buffer_memory_requirements(device, local_buffer, buffer_size, buffer_alignment, buffer_types), 0);

        const auto buffer_local_type = first_type(memory, buffer_types, false);
        ASSERT_NE(buffer_local_type, UINT32_MAX) << "memory types 0x" << std::hex << buffer_types;
        uint64_t buffer_local_memory = 0;
        ASSERT_EQ(host.allocate_memory(device, buffer_size, buffer_local_type, 0, 0, buffer_local_memory), 0);
        EXPECT_EQ(host.bind_buffer_memory(device, local_buffer, buffer_local_memory, 0), 0);

        const auto buffer_imported_memory = import_for(host, device, memory, buffer_size, buffer_types, buffer_storage);
        ASSERT_NE(buffer_imported_memory, 0u);
        EXPECT_EQ(host.bind_buffer_memory(device, imported_buffer, buffer_imported_memory, 0), 0);

        host.destroy_image(device, local_image);
        host.destroy_image(device, imported_image);
        host.destroy_buffer(device, local_buffer);
        host.destroy_buffer(device, imported_buffer);
        host.free_memory(device, image_local_memory);
        host.free_memory(device, image_imported_memory);
        host.free_memory(device, buffer_local_memory);
        host.free_memory(device, buffer_imported_memory);
        host.destroy_device(device);
        host.destroy_instance(instance);
    }
}
