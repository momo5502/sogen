#include "std_include.hpp"

#include <devices/vulkan_host.hpp>
#include <utils/serialization.hpp>
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    // Vulkan invalidates a recorded command buffer when an object its commands use is destroyed, and a restore tolerates a
    // replay that differs only for such a command buffer. Commands on objects that still exist leave it valid.
    TEST(VulkanHostTest, OnlyADestroyedObjectInvalidatesARecordedCommandBuffer)
    {
        utils::buffer_serializer snapshot{};
        uint64_t instance = 0;
        uint64_t device = 0;
        uint64_t kept = 0;
        uint64_t broken = 0;
        {
            vulkan_host host{};
            host.load(nullptr);
            if (!host.available() || host.create_instance({}, instance) != 0)
            {
                GTEST_SKIP() << "No host Vulkan driver";
            }

            std::array<uint64_t, 1> physical_devices{};
            uint32_t count = 0;
            host.enumerate_physical_devices(instance, physical_devices, count);
            if (count == 0 || host.create_device(physical_devices[0], nullptr, 0, nullptr, 0, 0, nullptr, 0, 0, false, device) != 0)
            {
                host.destroy_instance(instance);
                GTEST_SKIP() << "No host Vulkan device";
            }

            uint64_t pool = 0;
            uint64_t kept_event = 0;
            uint64_t destroyed_event = 0;
            ASSERT_EQ(host.create_command_pool(device, 0, 0, pool), VK_SUCCESS);
            ASSERT_EQ(host.allocate_command_buffer(device, pool, 0, kept), VK_SUCCESS);
            ASSERT_EQ(host.allocate_command_buffer(device, pool, 0, broken), VK_SUCCESS);
            ASSERT_EQ(host.create_event(device, 0, kept_event), VK_SUCCESS);
            ASSERT_EQ(host.create_event(device, 0, destroyed_event), VK_SUCCESS);

            // The way the bridge records: each command notes the objects it looks up, then is kept for the snapshot.
            const auto record = [&](const uint64_t command_buffer, const auto& command) {
                host.capture_references(command_buffer);
                const int32_t result = command();
                host.capture_references(0);
                constexpr std::array<std::byte, 1> recorded{};
                host.append_recorded_command(command_buffer, recorded, result);
                return result;
            };

            for (const auto& [command_buffer, event] : {std::pair{kept, kept_event}, std::pair{broken, destroyed_event}})
            {
                EXPECT_EQ(record(command_buffer, [&] { return host.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 0, 0); }),
                          VK_SUCCESS);
                EXPECT_EQ(record(command_buffer,
                                 [&] { return host.cmd_reset_event(command_buffer, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, true); }),
                          VK_SUCCESS);
                EXPECT_EQ(record(command_buffer, [&] { return host.end_command_buffer(command_buffer); }), VK_SUCCESS);
            }

            host.destroy_event(device, destroyed_event);
            host.quiesce();
            host.serialize(snapshot);
            host.destroy_device(device);
            host.destroy_instance(instance);
        }

        std::unordered_map<uint64_t, bool> invalidated{};
        vulkan_host restored{};
        restored.load(nullptr);
        utils::buffer_deserializer input{snapshot};
        restored.deserialize(input,
                             {.replay_commands = [&](const uint64_t command_buffer, std::span<const std::byte>, std::span<const int32_t>,
                                                     const bool was_invalidated) { invalidated[command_buffer] = was_invalidated; }});
        restored.destroy_device(device);
        restored.destroy_instance(instance);

        EXPECT_EQ(invalidated, (std::unordered_map<uint64_t, bool>{{kept, false}, {broken, true}}));
    }
}
