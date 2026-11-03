#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>

namespace sogen::test
{
    // The shim answers vkAllocateDescriptorSets itself for pools that cannot free single sets, counting only maxSets, and the
    // host allocates the sets later under the ids the guest already handed out. A host pool that refuses them must not turn
    // that answer into a failure.
    TEST(VulkanHostTest, DeferredDescriptorSetsKeepTheGuestIdsWhenTheirHostPoolIsFull)
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

        const std::array bindings{vulkan_host::descriptor_binding{.binding = 0,
                                                                  .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                                  .descriptor_count = 1,
                                                                  .stage_flags = VK_SHADER_STAGE_ALL,
                                                                  .binding_flags = 0}};
        uint64_t layout = 0;
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, bindings, layout), VK_SUCCESS);

        // Like DXVK's pools, this one cannot free single sets; with maxSets 1 every set after the first exceeds it.
        const std::array sizes{
            vulkan_host::descriptor_pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptor_count = 1}};
        uint64_t pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 1, 0, 0, sizes, pool), VK_SUCCESS);

        const std::array layouts{layout};
        for (uint64_t n = 1; n <= 3; ++n)
        {
            const std::array ids{gpu_bridge::guest_chosen_object_id | n};
            EXPECT_EQ(host.allocate_descriptor_sets_as(device, pool, layouts, {}, ids), VK_SUCCESS) << "set " << n;
        }

        // An id the guest already holds is refused instead of replacing the set behind it.
        const std::array taken{gpu_bridge::guest_chosen_object_id | 1};
        EXPECT_NE(host.allocate_descriptor_sets_as(device, pool, layouts, {}, taken), VK_SUCCESS);

        // A reset frees the pool's sets, also those in the extra host pools, and their ids with them.
        ASSERT_EQ(host.reset_descriptor_pool(device, pool, 0), VK_SUCCESS);
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, pool, layouts, {}, taken), VK_SUCCESS);

        host.destroy_descriptor_pool(device, pool);
        host.destroy_descriptor_set_layout(device, layout);
        host.destroy_device(device);
        host.destroy_instance(instance);
    }

    // The shim counts only maxSets, so the sets it answered for can need more descriptors of a type than the guest's pool
    // holds, over several sets or in one. They come from an overflow pool made with room for them.
    TEST(VulkanHostTest, DeferredDescriptorSetsGetRoomForMoreDescriptorsThanTheirPoolHolds)
    {
        vulkan_host host{};
        host.load(nullptr);
        if (!host.available())
        {
            GTEST_SKIP() << "No host Vulkan driver";
        }

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

        const auto uniform_buffers = [](const uint32_t count) {
            return std::array{vulkan_host::descriptor_binding{.binding = 0,
                                                              .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                              .descriptor_count = count,
                                                              .stage_flags = VK_SHADER_STAGE_ALL,
                                                              .binding_flags = 0}};
        };
        uint64_t one_buffer = 0;
        uint64_t two_buffers = 0;
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, uniform_buffers(1), one_buffer), VK_SUCCESS);
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, uniform_buffers(2), two_buffers), VK_SUCCESS);

        const std::array one_uniform_buffer{
            vulkan_host::descriptor_pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptor_count = 1}};

        // Two sets of one uniform buffer each, from a pool with room for two sets but one uniform buffer.
        uint64_t pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 2, 0, 0, one_uniform_buffer, pool), VK_SUCCESS);
        const std::array pair{one_buffer, one_buffer};
        const std::array pair_ids{gpu_bridge::guest_chosen_object_id | 1, gpu_bridge::guest_chosen_object_id | 2};
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, pool, pair, {}, pair_ids), VK_SUCCESS);

        // One set of two uniform buffers, from a pool with room for one set and one uniform buffer.
        uint64_t small_pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 1, 0, 0, one_uniform_buffer, small_pool), VK_SUCCESS);
        const std::array wide{two_buffers};
        const std::array wide_ids{gpu_bridge::guest_chosen_object_id | 3};
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, small_pool, wide, {}, wide_ids), VK_SUCCESS);

        host.destroy_descriptor_pool(device, small_pool);
        host.destroy_descriptor_pool(device, pool);
        host.destroy_descriptor_set_layout(device, two_buffers);
        host.destroy_descriptor_set_layout(device, one_buffer);
        host.destroy_device(device);
        host.destroy_instance(instance);
    }
}
