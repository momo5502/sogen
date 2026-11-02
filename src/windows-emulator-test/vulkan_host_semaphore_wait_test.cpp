#include <gtest/gtest.h>

#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <vk_feature_chain.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <vector>

namespace sogen::test
{
    // An async semaphore wait ends when the GPU signals, or with VK_TIMEOUT once its timeout passes, as vkWaitSemaphores
    // does. Destroying the semaphore or its device first ends it as a failed wait, so that no host thread is left inside
    // the driver with handles that are gone.
    TEST(VulkanHostTest, AsyncSemaphoreWaitEndsWhenSignaledTimedOutOrDestroyed)
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

        VkPhysicalDeviceVulkan12Features vulkan12{};
        vulkan12.timelineSemaphore = VK_TRUE;
        constexpr auto vulkan12_type = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        const gpu_bridge::feature_chain_record record{.s_type = vulkan12_type,
                                                      .body_size = static_cast<uint32_t>(gpu_bridge::feature_body_size(vulkan12_type))};
        std::vector<std::byte> features(sizeof(record) + record.body_size);
        std::memcpy(features.data(), &record, sizeof(record));
        std::memcpy(features.data() + sizeof(record), reinterpret_cast<const std::byte*>(&vulkan12) + gpu_bridge::feature_chain_header_size,
                    record.body_size);

        uint64_t device = 0;
        if (host.create_device(physical_devices[0], nullptr, 0, nullptr, 0, 0, features.data(), features.size(), 1, false, device) != 0)
        {
            host.destroy_instance(instance);
            GTEST_SKIP() << "No host Vulkan device with timeline semaphores";
        }

        uint64_t semaphore = 0;
        uint64_t other = 0;
        ASSERT_EQ(host.create_semaphore(device, 0, VK_SEMAPHORE_TYPE_TIMELINE, 0, semaphore), 0);
        ASSERT_EQ(host.create_semaphore(device, 0, VK_SEMAPHORE_TYPE_TIMELINE, 0, other), 0);

        const gpu_bridge::wait_semaphore_entry first{.semaphore = semaphore, .value = 1};
        std::promise<void> done;
        auto ended = done.get_future();
        const auto signaled = host.wait_semaphores_async(device, 0, &first, 1, UINT64_MAX, [&done] { done.set_value(); });
        EXPECT_EQ(ended.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
        EXPECT_EQ(signaled->load(), vulkan_host::semaphore_wait_pending);

        ASSERT_EQ(host.signal_semaphore(device, semaphore, 1), 0);
        ASSERT_EQ(ended.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(signaled->load(), VK_SUCCESS);

        const gpu_bridge::wait_semaphore_entry unsignaled{.semaphore = other, .value = 1};
        std::promise<void> timed_out;
        auto timeout_ended = timed_out.get_future();
        const auto finite = host.wait_semaphores_async(device, 0, &unsignaled, 1, 20'000'000, [&timed_out] { timed_out.set_value(); });
        ASSERT_EQ(timeout_ended.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(finite->load(), VK_TIMEOUT);

        const gpu_bridge::wait_semaphore_entry second{.semaphore = semaphore, .value = 2};
        const auto semaphore_destroyed = host.wait_semaphores_async(device, 0, &second, 1, UINT64_MAX, {});
        host.destroy_semaphore(device, semaphore);
        EXPECT_EQ(semaphore_destroyed->load(), VK_ERROR_INITIALIZATION_FAILED);

        const gpu_bridge::wait_semaphore_entry third{.semaphore = other, .value = 1};
        const auto device_destroyed = host.wait_semaphores_async(device, 0, &third, 1, UINT64_MAX, {});
        host.destroy_device(device);
        EXPECT_EQ(device_destroyed->load(), VK_ERROR_INITIALIZATION_FAILED);

        host.destroy_instance(instance);
    }
}
