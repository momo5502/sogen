#include "std_include.hpp"

#include <devices/vulkan_host.hpp>
#include <gpu_bridge_protocol.hpp>
#include <utils/serialization.hpp>
#include <vk_feature_chain.hpp>
#include <vulkan/vulkan_core.h>

namespace sogen::test
{
    namespace
    {
        // glslang -V --target-env vulkan1.0 -g0 of
        //   #version 450
        //   out gl_PerVertex { vec4 gl_Position; };
        //   void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
        constexpr std::array<uint32_t, 119> vertex_spirv{
            0x07230203, 0x00010000, 0x0008000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
            0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000000, 0x00000004, 0x6e69616d,
            0x00000000, 0x0000000a, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
            0x00000008, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000008, 0x00000000, 0x505f6c67, 0x7469736f,
            0x006e6f69, 0x00030005, 0x0000000a, 0x00000000, 0x00030047, 0x00000008, 0x00000002, 0x00050048, 0x00000008, 0x00000000,
            0x0000000b, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
            0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x0003001e, 0x00000008, 0x00000007, 0x00040020, 0x00000009, 0x00000003,
            0x00000008, 0x0004003b, 0x00000009, 0x0000000a, 0x00000003, 0x00040015, 0x0000000b, 0x00000020, 0x00000001, 0x0004002b,
            0x0000000b, 0x0000000c, 0x00000000, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000, 0x0004002b, 0x00000006, 0x0000000e,
            0x3f800000, 0x0007002c, 0x00000007, 0x0000000f, 0x0000000d, 0x0000000d, 0x0000000d, 0x0000000e, 0x00040020, 0x00000010,
            0x00000003, 0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041,
            0x00000010, 0x00000011, 0x0000000a, 0x0000000c, 0x0003003e, 0x00000011, 0x0000000f, 0x000100fd, 0x00010038,
        };

        // and of
        //   #version 450
        //   layout(constant_id = 0) const float shade = 1.0;
        //   layout(location = 0) out vec4 color;
        //   void main() { color = vec4(shade); }
        constexpr std::array<uint32_t, 91> fragment_spirv{
            0x07230203, 0x00010000, 0x0008000b, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
            0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
            0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
            0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x6f6c6f63, 0x00000072, 0x00040005, 0x0000000a, 0x64616873, 0x00000065,
            0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000a, 0x00000001, 0x00000000, 0x00020013, 0x00000002,
            0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
            0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040032, 0x00000006,
            0x0000000a, 0x3f800000, 0x00070033, 0x00000007, 0x0000000b, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00050036,
            0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009, 0x0000000b, 0x000100fd,
            0x00010038,
        };

        constexpr std::array<uint32_t, 1> color_formats{VK_FORMAT_R8G8B8A8_UNORM};

        template <typename Features>
        void append_feature_record(std::vector<std::byte>& blob, const VkStructureType type, const Features& features)
        {
            const gpu_bridge::feature_chain_record record{.s_type = static_cast<uint32_t>(type),
                                                          .body_size = static_cast<uint32_t>(gpu_bridge::feature_body_size(type))};
            const auto offset = blob.size();
            blob.resize(offset + sizeof(record) + record.body_size);
            std::memcpy(blob.data() + offset, &record, sizeof(record));
            std::memcpy(blob.data() + offset + sizeof(record),
                        reinterpret_cast<const std::byte*>(&features) + gpu_bridge::feature_chain_header_size, record.body_size);
        }

        uint64_t create_pipeline_library_device(vulkan_host& host, const uint64_t instance)
        {
            using namespace std::string_view_literals;
            constexpr auto extensions = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME "\0" VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME "\0"sv;
            constexpr uint32_t extension_count = 2;

            VkPhysicalDeviceVulkan13Features vulkan13{};
            vulkan13.dynamicRendering = VK_TRUE;
            VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT pipeline_library{};
            pipeline_library.graphicsPipelineLibrary = VK_TRUE;

            std::vector<std::byte> features{};
            append_feature_record(features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, vulkan13);
            append_feature_record(features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, pipeline_library);
            constexpr uint32_t feature_count = 2;

            std::array<uint64_t, 8> physical_devices{};
            uint32_t count = 0;
            host.enumerate_physical_devices(instance, physical_devices, count);
            for (const uint64_t physical_device : std::span{physical_devices}.first(std::min<size_t>(count, physical_devices.size())))
            {
                uint64_t device = 0;
                if (host.create_device(physical_device, nullptr, 0, extensions.data(), extensions.size(), extension_count, features.data(),
                                       features.size(), feature_count, false, device) == VK_SUCCESS)
                {
                    return device;
                }
            }
            return 0;
        }

        int32_t run_here(const std::function<int32_t()>& create)
        {
            return create();
        }

        int32_t create_library(vulkan_host& host, const uint64_t device, const VkGraphicsPipelineLibraryFlagBitsEXT part,
                               const uint64_t layout, const std::span<const vulkan_host::pipeline_stage> stages, uint64_t& library)
        {
            vulkan_host::depth_stencil_state depth_stencil{};
            depth_stencil.present = part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            return host.create_graphics_pipeline({.device = device,
                                                  .pipeline_layout = layout,
                                                  .stages = stages,
                                                  .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
                                                  .library_parts = static_cast<uint32_t>(part),
                                                  .depth_stencil = depth_stencil,
                                                  .color_formats = color_formats,
                                                  .primitive_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                  .rasterization_stream = UINT32_MAX},
                                                 run_here, library);
        }

        // The way the shim passes a stage's code inline: in a shader module made for the call and destroyed after it.
        int32_t create_shader_library(vulkan_host& host, const uint64_t device, const uint64_t layout, const VkShaderStageFlagBits stage,
                                      const std::span<const uint32_t> code, uint64_t& library)
        {
            uint64_t module = 0;
            if (const auto result = host.create_shader_module(device, 0, code.data(), code.size_bytes(), module); result != VK_SUCCESS)
            {
                return result;
            }

            const std::array stages{vulkan_host::pipeline_stage{.stage = static_cast<uint32_t>(stage),
                                                                .source = {.module = module, .identifier = {}},
                                                                .spec = {.entries = {}, .data = {}}}};
            const auto part = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                                                                  : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            const auto result = create_library(host, device, part, layout, stages, library);
            host.destroy_shader_module(device, module);
            return result;
        }

        int32_t link_libraries(vulkan_host& host, const uint64_t device, const uint64_t layout, const std::span<const uint64_t> libraries,
                               uint64_t& pipeline)
        {
            return host.create_graphics_pipeline(
                {.device = device, .pipeline_layout = layout, .libraries = libraries, .color_formats = color_formats}, run_here, pipeline);
        }
    }

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

    // A graphics pipeline library of only the interface parts, and a pipeline linked from libraries, have no shader stages of
    // their own. A linked pipeline stays valid after a library it links is destroyed.
    TEST(VulkanHostTest, ASnapshotKeepsPipelineLibrariesAndThePipelinesLinkedFromThem)
    {
        utils::buffer_serializer snapshot{};
        uint64_t instance = 0;
        uint64_t device = 0;
        uint64_t layout = 0;
        uint64_t vertex_input = 0;
        uint64_t pre_rasterization = 0;
        uint64_t fragment_output = 0;
        uint64_t linked = 0;
        {
            vulkan_host host{};
            host.load(nullptr);
            if (!host.available() || host.create_instance({}, instance) != VK_SUCCESS)
            {
                GTEST_SKIP() << "No host Vulkan driver";
            }

            device = create_pipeline_library_device(host, instance);
            if (device == 0)
            {
                host.destroy_instance(instance);
                GTEST_SKIP() << "No host Vulkan device with graphics pipeline libraries";
            }

            uint64_t fragment_shader = 0;
            ASSERT_EQ(host.create_pipeline_layout(device, 0, 0, 0, {}, layout), VK_SUCCESS);
            ASSERT_EQ(create_library(host, device, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, 0, {}, vertex_input),
                      VK_SUCCESS);
            ASSERT_EQ(create_shader_library(host, device, layout, VK_SHADER_STAGE_VERTEX_BIT, vertex_spirv, pre_rasterization), VK_SUCCESS);
            ASSERT_EQ(create_shader_library(host, device, layout, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_spirv, fragment_shader),
                      VK_SUCCESS);
            ASSERT_EQ(create_library(host, device, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, 0, {}, fragment_output),
                      VK_SUCCESS);
            ASSERT_EQ(
                link_libraries(host, device, layout, std::array{vertex_input, pre_rasterization, fragment_shader, fragment_output}, linked),
                VK_SUCCESS);
            host.destroy_pipeline(device, fragment_shader);

            host.quiesce();
            ASSERT_NO_THROW(host.serialize(snapshot));
            host.destroy_device(device);
            host.destroy_instance(instance);
        }

        vulkan_host restored{};
        restored.load(nullptr);
        utils::buffer_deserializer input{snapshot};
        restored.deserialize(input, {});

        uint64_t pool = 0;
        uint64_t command_buffer = 0;
        ASSERT_EQ(restored.create_command_pool(device, 0, 0, pool), VK_SUCCESS);
        ASSERT_EQ(restored.allocate_command_buffer(device, pool, 0, command_buffer), VK_SUCCESS);
        ASSERT_EQ(restored.begin_command_buffer(command_buffer, 0, false, 0, {}, 0, 0, 0, 0), VK_SUCCESS);
        EXPECT_EQ(restored.cmd_bind_pipeline(command_buffer, linked, VK_PIPELINE_BIND_POINT_GRAPHICS), VK_SUCCESS);

        uint64_t fragment_shader = 0;
        uint64_t relinked = 0;
        ASSERT_EQ(create_shader_library(restored, device, layout, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_spirv, fragment_shader),
                  VK_SUCCESS);
        EXPECT_EQ(link_libraries(restored, device, layout, std::array{vertex_input, pre_rasterization, fragment_shader, fragment_output},
                                 relinked),
                  VK_SUCCESS);

        restored.destroy_device(device);
        restored.destroy_instance(instance);
    }
}
