#include "std_include.hpp"

#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    namespace
    {
        // A host device with a set layout of one uniform buffer, for the pool cases below.
        struct descriptor_host
        {
            vulkan_host host{};
            uint64_t instance{};
            uint64_t device{};
            uint64_t layout{};

            descriptor_host() = default;
            descriptor_host(const descriptor_host&) = delete;
            descriptor_host& operator=(const descriptor_host&) = delete;
            descriptor_host(descriptor_host&&) = delete;
            descriptor_host& operator=(descriptor_host&&) = delete;

            ~descriptor_host()
            {
                if (this->device)
                {
                    this->host.destroy_device(this->device);
                }
                if (this->instance)
                {
                    this->host.destroy_instance(this->instance);
                }
            }

            // False without a host Vulkan driver and device.
            bool open()
            {
                this->host.load(nullptr);
                if (!this->host.available() || this->host.create_instance({}, this->instance) != 0)
                {
                    return false;
                }

                std::array<uint64_t, 1> physical_devices{};
                uint32_t count = 0;
                this->host.enumerate_physical_devices(this->instance, physical_devices, count);
                if (count == 0 ||
                    this->host.create_device(physical_devices[0], nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, false, this->device) != 0)
                {
                    return false;
                }

                this->layout = this->make_layout();
                return this->layout != 0;
            }

            uint64_t make_layout()
            {
                const std::array bindings{vulkan_host::descriptor_binding{.binding = 0,
                                                                          .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                                          .descriptor_count = 1,
                                                                          .stage_flags = VK_SHADER_STAGE_ALL,
                                                                          .binding_flags = 0}};
                uint64_t made = 0;
                return this->host.create_descriptor_set_layout(this->device, 0, bindings, {}, made) == VK_SUCCESS ? made : 0;
            }

            // A pool with room for max_sets sets of the layout.
            uint64_t make_pool(const uint32_t max_sets, const uint32_t flags = 0)
            {
                const std::array sizes{
                    vulkan_host::descriptor_pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptor_count = max_sets}};
                uint64_t pool = 0;
                return this->host.create_descriptor_pool(this->device, max_sets, flags, 0, sizes, {}, pool) == VK_SUCCESS ? pool : 0;
            }

            // A set allocated the way the shim defers it, under the guest's own id n.
            int32_t allocate(const uint64_t pool, const uint64_t n)
            {
                return this->allocate_with(pool, n, this->layout);
            }

            int32_t allocate_with(const uint64_t pool, const uint64_t n, const uint64_t with_layout)
            {
                const std::array layouts{with_layout};
                const std::array ids{gpu_bridge::guest_chosen_object_id | n};
                return this->host.allocate_descriptor_sets_as(this->device, pool, layouts, {}, ids);
            }

            int32_t reset(const uint64_t pool)
            {
                return this->host.reset_descriptor_pool(this->device, pool, 0);
            }
        };
    }

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
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, bindings, {}, layout), VK_SUCCESS);

        // Like DXVK's pools, this one cannot free single sets; with maxSets 1 every set after the first exceeds it.
        const std::array sizes{
            vulkan_host::descriptor_pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptor_count = 1}};
        uint64_t pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 1, 0, 0, sizes, {}, pool), VK_SUCCESS);

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

        // A pool that frees single sets never spills, as the guest frees a set through the pool it came from: a set allocated
        // under a guest id (a snapshot restore) gets what the driver gives a synchronous allocation from the same pool.
        std::array<uint64_t, 2> freeable_pools{};
        for (auto& freeable : freeable_pools)
        {
            ASSERT_EQ(host.create_descriptor_pool(device, 1, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 0, sizes, {}, freeable),
                      VK_SUCCESS);
        }

        std::array<uint64_t, 1> synchronous_set{};
        uint32_t synchronous_count = 0;
        int32_t synchronous_result = VK_SUCCESS;
        for (int n = 0; n < 2; ++n)
        {
            synchronous_result = host.allocate_descriptor_sets(device, freeable_pools[0], layouts, {}, synchronous_set, synchronous_count);
        }

        const std::array first{gpu_bridge::guest_chosen_object_id | 4};
        const std::array second{gpu_bridge::guest_chosen_object_id | 5};
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, freeable_pools[1], layouts, {}, first), VK_SUCCESS);
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, freeable_pools[1], layouts, {}, second), synchronous_result);

        for (const auto freeable : freeable_pools)
        {
            host.destroy_descriptor_pool(device, freeable);
        }
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
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, uniform_buffers(1), {}, one_buffer), VK_SUCCESS);
        ASSERT_EQ(host.create_descriptor_set_layout(device, 0, uniform_buffers(2), {}, two_buffers), VK_SUCCESS);

        const std::array one_uniform_buffer{
            vulkan_host::descriptor_pool_size{.descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptor_count = 1}};

        // Two sets of one uniform buffer each, from a pool with room for two sets but one uniform buffer.
        uint64_t pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 2, 0, 0, one_uniform_buffer, {}, pool), VK_SUCCESS);
        const std::array pair{one_buffer, one_buffer};
        const std::array pair_ids{gpu_bridge::guest_chosen_object_id | 1, gpu_bridge::guest_chosen_object_id | 2};
        EXPECT_EQ(host.allocate_descriptor_sets_as(device, pool, pair, {}, pair_ids), VK_SUCCESS);

        // One set of two uniform buffers, from a pool with room for one set and one uniform buffer.
        uint64_t small_pool = 0;
        ASSERT_EQ(host.create_descriptor_pool(device, 1, 0, 0, one_uniform_buffer, {}, small_pool), VK_SUCCESS);
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

    // A reset frees the ids of the pool's own sets and no others.
    TEST(VulkanHostTest, APoolResetDropsOnlyItsOwnSets)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto a = vk.make_pool(4);
        const auto b = vk.make_pool(4);
        ASSERT_NE(a, 0u);
        ASSERT_NE(b, 0u);
        ASSERT_EQ(vk.allocate(a, 1), VK_SUCCESS);
        ASSERT_EQ(vk.allocate(a, 2), VK_SUCCESS);
        ASSERT_EQ(vk.allocate(b, 3), VK_SUCCESS);

        ASSERT_EQ(vk.reset(a), VK_SUCCESS);
        EXPECT_EQ(vk.allocate(a, 1), VK_SUCCESS) << "a reset frees the ids of its sets";
        EXPECT_EQ(vk.allocate(a, 2), VK_SUCCESS);
        EXPECT_NE(vk.allocate(b, 3), VK_SUCCESS) << "the other pool's set stays";

        vk.host.destroy_descriptor_pool(vk.device, a);
        vk.host.destroy_descriptor_pool(vk.device, b);
    }

    // Like DXVK's, a pool reset every frame hands its kept sets to the next frame's allocations.
    TEST(VulkanHostTest, APoolResetEveryFrameKeepsAllocating)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(8);
        ASSERT_NE(pool, 0u);
        for (int frame = 0; frame < 50; ++frame)
        {
            for (uint64_t n = 1; n <= 8; ++n)
            {
                ASSERT_EQ(vk.allocate(pool, n), VK_SUCCESS) << "frame " << frame << " set " << n;
            }
            ASSERT_EQ(vk.reset(pool), VK_SUCCESS) << "frame " << frame;
        }
        vk.host.destroy_descriptor_pool(vk.device, pool);
    }

    // With maxSets 1 every set after the first comes from an overflow pool; more kept sets than twice maxSets are freed.
    TEST(VulkanHostTest, SetsInOverflowPoolsResetWithTheirPool)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(1);
        ASSERT_NE(pool, 0u);
        for (int frame = 0; frame < 3; ++frame)
        {
            for (uint64_t n = 1; n <= 3; ++n)
            {
                EXPECT_EQ(vk.allocate(pool, n), VK_SUCCESS) << "frame " << frame << " set " << n;
            }
            ASSERT_EQ(vk.reset(pool), VK_SUCCESS) << "frame " << frame;
        }
        vk.host.destroy_descriptor_pool(vk.device, pool);
    }

    // A pool that can free single sets allocates through vkAllocateDescriptorSets' own call, and a freed set leaves it.
    TEST(VulkanHostTest, AFreedSetLeavesItsPool)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(4, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
        ASSERT_NE(pool, 0u);
        const std::array layouts{vk.layout, vk.layout};
        std::array<uint64_t, 2> sets{};
        uint32_t count = 0;
        ASSERT_EQ(vk.host.allocate_descriptor_sets(vk.device, pool, layouts, {}, sets, count), VK_SUCCESS);
        ASSERT_EQ(count, 2u);

        const std::array freed{sets[0]};
        ASSERT_EQ(vk.host.free_descriptor_sets(vk.device, pool, freed), VK_SUCCESS);
        ASSERT_EQ(vk.reset(pool), VK_SUCCESS);
        ASSERT_EQ(vk.host.allocate_descriptor_sets(vk.device, pool, layouts, {}, sets, count), VK_SUCCESS);
        EXPECT_EQ(count, 2u);
        vk.host.destroy_descriptor_pool(vk.device, pool);
    }

    // A free takes a set out of its pool's list wherever it sits there, so the pool's reset drops exactly the sets left.
    TEST(VulkanHostTest, SetsFreedInAnyOrderLeaveTheirPool)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(4, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT);
        ASSERT_NE(pool, 0u);
        const std::array layouts{vk.layout, vk.layout, vk.layout, vk.layout};
        std::array<uint64_t, 4> sets{};
        uint32_t count = 0;
        ASSERT_EQ(vk.host.allocate_descriptor_sets(vk.device, pool, layouts, {}, sets, count), VK_SUCCESS);
        ASSERT_EQ(count, 4u);

        // The first set, then the last one, which took the first one's place in the list, then one from the middle.
        constexpr std::array<size_t, 3> freed_order{0, 3, 1};
        for (const auto index : freed_order)
        {
            const std::array freed{sets[index]};
            ASSERT_EQ(vk.host.free_descriptor_sets(vk.device, pool, freed), VK_SUCCESS);
        }

        ASSERT_EQ(vk.reset(pool), VK_SUCCESS);
        const std::array left{sets[2]};
        EXPECT_EQ(vk.host.free_descriptor_sets(vk.device, pool, left), VK_ERROR_INITIALIZATION_FAILED) << "the reset dropped it";
        ASSERT_EQ(vk.host.allocate_descriptor_sets(vk.device, pool, layouts, {}, sets, count), VK_SUCCESS);
        EXPECT_EQ(count, 4u);
        vk.host.destroy_descriptor_pool(vk.device, pool);
    }

    // A kept set could not be updated once its layout is destroyed, so it is not handed out again.
    TEST(VulkanHostTest, AKeptSetOfADestroyedLayoutIsNotHandedOut)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(4);
        const auto other = vk.make_layout();
        ASSERT_NE(pool, 0u);
        ASSERT_NE(other, 0u);
        ASSERT_EQ(vk.allocate_with(pool, 1, other), VK_SUCCESS);
        ASSERT_EQ(vk.allocate_with(pool, 2, other), VK_SUCCESS);
        ASSERT_EQ(vk.reset(pool), VK_SUCCESS);

        vk.host.destroy_descriptor_set_layout(vk.device, other);
        EXPECT_NE(vk.allocate_with(pool, 1, other), VK_SUCCESS) << "the layout is gone";
        EXPECT_EQ(vk.allocate(pool, 1), VK_SUCCESS) << "a live layout still allocates";
        vk.host.destroy_descriptor_pool(vk.device, pool);
    }

    // Destroying a pool frees the ids of its sets.
    TEST(VulkanHostTest, DestroyingAPoolFreesItsSetIds)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto first = vk.make_pool(4);
        ASSERT_NE(first, 0u);
        ASSERT_EQ(vk.allocate(first, 5), VK_SUCCESS);
        ASSERT_EQ(vk.reset(first), VK_SUCCESS);
        ASSERT_EQ(vk.allocate(first, 5), VK_SUCCESS);
        vk.host.destroy_descriptor_pool(vk.device, first);

        const auto second = vk.make_pool(4);
        ASSERT_NE(second, 0u);
        EXPECT_EQ(vk.allocate(second, 5), VK_SUCCESS);
        vk.host.destroy_descriptor_pool(vk.device, second);
    }

    // A reset keeps a deferred-only pool's host sets, which still fill its host pool. A synchronous allocation the guest's own
    // count of the pool allows gets its set anyway.
    TEST(VulkanHostTest, ASynchronousAllocationGetsTheRoomKeptSetsTake)
    {
        descriptor_host vk{};
        if (!vk.open())
        {
            GTEST_SKIP() << "No host Vulkan device";
        }

        const auto pool = vk.make_pool(1);
        ASSERT_NE(pool, 0u);
        ASSERT_EQ(vk.allocate(pool, 1), VK_SUCCESS);
        ASSERT_EQ(vk.reset(pool), VK_SUCCESS);

        const std::array layouts{vk.layout};
        std::array<uint64_t, 1> sets{};
        uint32_t count = 0;
        EXPECT_EQ(vk.host.allocate_descriptor_sets(vk.device, pool, layouts, {}, sets, count), VK_SUCCESS);
        EXPECT_EQ(count, 1u);

        vk.host.destroy_descriptor_pool(vk.device, pool);
    }
}
