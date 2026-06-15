#include "vulkan_host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <gpu_bridge_protocol.hpp>
#include <vk_feature_chain.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sogen
{
    namespace
    {
#ifdef _WIN32
        using library_handle = HMODULE;

        library_handle load_library(const char* name)
        {
            return ::LoadLibraryA(name);
        }

        void* get_symbol(library_handle handle, const char* name)
        {
            return reinterpret_cast<void*>(::GetProcAddress(handle, name));
        }

        void free_library(library_handle handle)
        {
            ::FreeLibrary(handle);
        }

        constexpr std::array<const char*, 1> vulkan_loader_names{"vulkan-1.dll"};
#else
        using library_handle = void*;

        library_handle load_library(const char* name)
        {
            return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }

        void* get_symbol(library_handle handle, const char* name)
        {
            return ::dlsym(handle, name);
        }

        void free_library(library_handle handle)
        {
            ::dlclose(handle);
        }

#if defined(__APPLE__)
        constexpr std::array<const char*, 3> vulkan_loader_names{"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib"};
#else
        constexpr std::array<const char*, 2> vulkan_loader_names{"libvulkan.so.1", "libvulkan.so"};
#endif
#endif

        // VkPhysicalDeviceProperties crosses the 32/64-bit ABI boundary unchanged except for
        // VkPhysicalDeviceLimits::minMemoryMapAlignment, the struct's only size_t (8 bytes on the
        // 64-bit host, 4 bytes on the 32-bit WoW64 guest). Members before it are ABI-identical;
        // members after it keep the same bytes but sit at lower offsets on the guest. Repack the
        // host struct into the guest's 32-bit layout so DXVK reads e.g. framebuffer*SampleCounts
        // from the right offset.
        size_t repack_properties_for_wow64(const VkPhysicalDeviceProperties& src, void* out, size_t out_size)
        {
            if constexpr (sizeof(size_t) != 8)
            {
                (void)src;
                (void)out;
                (void)out_size;
                return 0;
            }
            else
            {
                constexpr size_t guest_size_t = sizeof(uint32_t);
                // Guest offset where minMemoryMapAlignment begins: it follows viewportSubPixelBits and,
                // being a 4-byte size_t on the guest, needs no 8-byte padding (unlike on the host, where
                // offsetof(minMemoryMapAlignment) is rounded up). Anchor on the preceding field's end.
                constexpr size_t limits_head = offsetof(VkPhysicalDeviceLimits, viewportSubPixelBits) + sizeof(uint32_t);
                constexpr size_t host_limits_tail = offsetof(VkPhysicalDeviceLimits, minTexelBufferOffsetAlignment);
                constexpr size_t guest_limits_tail = (limits_head + guest_size_t + 7) & ~static_cast<size_t>(7);
                constexpr size_t limits_tail_size = sizeof(VkPhysicalDeviceLimits) - host_limits_tail;
                constexpr size_t guest_limits_size = guest_limits_tail + limits_tail_size;

                constexpr size_t props_head = offsetof(VkPhysicalDeviceProperties, limits);
                constexpr size_t sparse_size = sizeof(VkPhysicalDeviceSparseProperties);
                constexpr size_t guest_props_size = props_head + guest_limits_size + sparse_size;

                std::array<std::byte, sizeof(VkPhysicalDeviceProperties)> buffer{};
                const auto* src_bytes = reinterpret_cast<const std::byte*>(&src);
                std::byte* dst = buffer.data();

                // header + limits up to (but not including) minMemoryMapAlignment
                std::memcpy(dst, src_bytes, props_head + limits_head);

                const auto map_alignment = static_cast<uint32_t>(src.limits.minMemoryMapAlignment);
                std::memcpy(dst + props_head + limits_head, &map_alignment, sizeof(map_alignment));

                // limits tail (minTexelBufferOffsetAlignment .. end) then sparseProperties
                std::memcpy(dst + props_head + guest_limits_tail, src_bytes + props_head + host_limits_tail, limits_tail_size);
                std::memcpy(dst + props_head + guest_limits_size, src_bytes + props_head + sizeof(VkPhysicalDeviceLimits), sparse_size);

                const size_t copy_size = std::min(out_size, guest_props_size);
                std::memcpy(out, buffer.data(), copy_size);
                return copy_size;
            }
        }
    }

    struct vulkan_host::impl
    {
        library_handle loader{};
        PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
        PFN_vkCreateInstance create_instance{};
        PFN_vkEnumerateInstanceVersion enumerate_instance_version{};

        struct instance_data
        {
            VkInstance handle{};
            PFN_vkDestroyInstance destroy_instance{};
            PFN_vkEnumeratePhysicalDevices enumerate_physical_devices{};
            PFN_vkGetPhysicalDeviceProperties get_physical_device_properties{};
            PFN_vkGetPhysicalDeviceFormatProperties get_physical_device_format_properties{};
            PFN_vkGetPhysicalDeviceImageFormatProperties get_physical_device_image_format_properties{};
            PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
            PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties{};
            PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2{};
            PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2{};
            PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties{};
            PFN_vkCreateDevice create_device{};
            PFN_vkGetDeviceProcAddr get_device_proc_addr{};
        };

        struct physical_device_data
        {
            VkPhysicalDevice handle{};
            uint64_t instance_id{};
        };

        struct device_data
        {
            VkDevice handle{};
            uint64_t instance_id{};
            VkPhysicalDevice physical_device{}; // the device this was created from (for memory queries)
            uint32_t queue_family_index{};      // the single family this device was created with
            PFN_vkDestroyDevice destroy_device{};
            PFN_vkGetDeviceQueue get_device_queue{};
            PFN_vkQueueWaitIdle queue_wait_idle{};
            PFN_vkDeviceWaitIdle device_wait_idle{};
            PFN_vkCreateCommandPool create_command_pool{};
            PFN_vkDestroyCommandPool destroy_command_pool{};
            PFN_vkAllocateCommandBuffers allocate_command_buffers{};
            PFN_vkFreeCommandBuffers free_command_buffers{};
            PFN_vkBeginCommandBuffer begin_command_buffer{};
            PFN_vkEndCommandBuffer end_command_buffer{};
            PFN_vkResetCommandPool reset_command_pool{};
            PFN_vkResetCommandBuffer reset_command_buffer{};
            PFN_vkCreateFence create_fence{};
            PFN_vkDestroyFence destroy_fence{};
            PFN_vkResetFences reset_fences{};
            PFN_vkGetFenceStatus get_fence_status{};
            PFN_vkCreateEvent create_event{};
            PFN_vkDestroyEvent destroy_event{};
            PFN_vkGetEventStatus get_event_status{};
            PFN_vkSetEvent set_event{};
            PFN_vkResetEvent reset_event{};
            PFN_vkCreateSemaphore create_semaphore{};
            PFN_vkDestroySemaphore destroy_semaphore{};
            PFN_vkGetSemaphoreCounterValue get_semaphore_counter_value{};
            PFN_vkSignalSemaphore signal_semaphore{};
            PFN_vkWaitSemaphores wait_semaphores{};
            PFN_vkGetBufferDeviceAddress get_buffer_device_address{};
            PFN_vkQueueSubmit queue_submit{};
            PFN_vkQueueSubmit2 queue_submit2{};
            PFN_vkAllocateMemory allocate_memory{};
            PFN_vkFreeMemory free_memory{};
            PFN_vkMapMemory map_memory{};
            PFN_vkUnmapMemory unmap_memory{};
            PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges{};
            PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges{};
            PFN_vkCreateBuffer create_buffer{};
            PFN_vkDestroyBuffer destroy_buffer{};
            PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
            PFN_vkBindBufferMemory bind_buffer_memory{};
            PFN_vkCmdFillBuffer cmd_fill_buffer{};
            PFN_vkCreateImage create_image{};
            PFN_vkDestroyImage destroy_image{};
            PFN_vkGetImageMemoryRequirements get_image_memory_requirements{};
            PFN_vkGetImageSubresourceLayout get_image_subresource_layout{};
            PFN_vkBindImageMemory bind_image_memory{};
            PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
            PFN_vkCmdClearColorImage cmd_clear_color_image{};
            PFN_vkCmdClearDepthStencilImage cmd_clear_depth_stencil_image{};
            PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};
            PFN_vkCmdResolveImage cmd_resolve_image{};
            PFN_vkCmdUpdateBuffer cmd_update_buffer{};
            PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image{};
            PFN_vkCmdCopyImage cmd_copy_image{};
            PFN_vkCmdCopyBuffer cmd_copy_buffer{};
            PFN_vkCreateSampler create_sampler{};
            PFN_vkDestroySampler destroy_sampler{};
            PFN_vkCreateShaderModule create_shader_module{};
            PFN_vkDestroyShaderModule destroy_shader_module{};
            PFN_vkCreateImageView create_image_view{};
            PFN_vkDestroyImageView destroy_image_view{};
            PFN_vkCreateBufferView create_buffer_view{};
            PFN_vkDestroyBufferView destroy_buffer_view{};
            PFN_vkCreateQueryPool create_query_pool{};
            PFN_vkDestroyQueryPool destroy_query_pool{};
            PFN_vkResetQueryPool reset_query_pool{};
            PFN_vkGetQueryPoolResults get_query_pool_results{};
            PFN_vkCmdResetQueryPool cmd_reset_query_pool{};
            PFN_vkCmdBeginQuery cmd_begin_query{};
            PFN_vkCmdEndQuery cmd_end_query{};
            PFN_vkCmdWriteTimestamp cmd_write_timestamp{};
            PFN_vkCreateRenderPass create_render_pass{};
            PFN_vkDestroyRenderPass destroy_render_pass{};
            PFN_vkCreateFramebuffer create_framebuffer{};
            PFN_vkDestroyFramebuffer destroy_framebuffer{};
            PFN_vkCreatePipelineLayout create_pipeline_layout{};
            PFN_vkDestroyPipelineLayout destroy_pipeline_layout{};
            PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout{};
            PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout{};
            PFN_vkCreateDescriptorPool create_descriptor_pool{};
            PFN_vkDestroyDescriptorPool destroy_descriptor_pool{};
            PFN_vkAllocateDescriptorSets allocate_descriptor_sets{};
            PFN_vkUpdateDescriptorSets update_descriptor_sets{};
            PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets{};
            PFN_vkCreateGraphicsPipelines create_graphics_pipelines{};
            PFN_vkCreateComputePipelines create_compute_pipelines{};
            PFN_vkDestroyPipeline destroy_pipeline{};
            PFN_vkCmdBeginRenderPass cmd_begin_render_pass{};
            PFN_vkCmdBeginRendering cmd_begin_rendering{};
            PFN_vkCmdEndRendering cmd_end_rendering{};
            PFN_vkCmdExecuteCommands cmd_execute_commands{};
            PFN_vkCmdBindPipeline cmd_bind_pipeline{};
            PFN_vkCmdDispatch cmd_dispatch{};
            PFN_vkCmdDispatchIndirect cmd_dispatch_indirect{};
            PFN_vkCmdDraw cmd_draw{};
            PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers{};
            PFN_vkCmdBindVertexBuffers2 cmd_bind_vertex_buffers2{};
            PFN_vkCmdBindIndexBuffer cmd_bind_index_buffer{};
            PFN_vkCmdDrawIndexed cmd_draw_indexed{};
            PFN_vkCmdEndRenderPass cmd_end_render_pass{};
            PFN_vkCmdPushConstants cmd_push_constants{};
            PFN_vkCmdSetViewport cmd_set_viewport{};
            PFN_vkCmdSetViewportWithCount cmd_set_viewport_with_count{};
            PFN_vkCmdSetScissor cmd_set_scissor{};
            PFN_vkCmdSetScissorWithCount cmd_set_scissor_with_count{};
            PFN_vkCmdSetDepthBias cmd_set_depth_bias{};
            PFN_vkCmdSetBlendConstants cmd_set_blend_constants{};
            PFN_vkCmdSetDepthBounds cmd_set_depth_bounds{};
            PFN_vkCmdSetLineWidth cmd_set_line_width{};
            PFN_vkCmdSetStencilCompareMask cmd_set_stencil_compare_mask{};
            PFN_vkCmdSetStencilWriteMask cmd_set_stencil_write_mask{};
            PFN_vkCmdSetStencilReference cmd_set_stencil_reference{};
            PFN_vkCmdSetStencilOp cmd_set_stencil_op{};
            PFN_vkCmdSetCullMode cmd_set_cull_mode{};
            PFN_vkCmdSetFrontFace cmd_set_front_face{};
            PFN_vkCmdSetPrimitiveTopology cmd_set_primitive_topology{};
            PFN_vkCmdSetDepthTestEnable cmd_set_depth_test_enable{};
            PFN_vkCmdSetDepthWriteEnable cmd_set_depth_write_enable{};
            PFN_vkCmdSetDepthCompareOp cmd_set_depth_compare_op{};
            PFN_vkCmdSetDepthBoundsTestEnable cmd_set_depth_bounds_test_enable{};
            PFN_vkCmdSetStencilTestEnable cmd_set_stencil_test_enable{};
            PFN_vkCmdSetRasterizerDiscardEnable cmd_set_rasterizer_discard_enable{};
            PFN_vkCmdSetDepthBiasEnable cmd_set_depth_bias_enable{};
            PFN_vkCmdSetPrimitiveRestartEnable cmd_set_primitive_restart_enable{};
        };

        // A guest-side window-system surface. There is no real host VkSurfaceKHR (the host driver may
        // lack Win32 WSI); the bridge only needs the guest HWND to present readback pixels to.
        struct surface_data
        {
            uint64_t hwnd{};
        };

        // A swapchain is modeled as N offscreen images plus a host-visible readback buffer. "Presenting"
        // copies the chosen image into the readback buffer; the bridge then hands those pixels to the
        // guest window via the UI backend. No real presentation engine is involved.
        struct swapchain_data
        {
            uint64_t device_id{};
            uint64_t hwnd{};
            uint32_t width{};
            uint32_t height{};
            VkFormat format{};
            std::vector<uint64_t> image_ids{}; // entries in `images`, owned by this swapchain
            std::vector<VkDeviceMemory> image_memory{};
            VkBuffer readback_buffer{};
            VkDeviceMemory readback_memory{};
            VkCommandPool present_pool{};
            VkCommandBuffer present_cmd{};
            VkFence present_fence{};
            uint32_t next_image{};
            // Async present: the readback copy of the most recent frame is submitted but not waited on
            // here; it is drained on the *next* present (by which point the GPU has finished it), so the
            // host thread never blocks on vkQueueWaitIdle inline. Present is therefore one frame behind.
            bool present_in_flight{};
        };

        std::unordered_map<uint64_t, instance_data> instances;
        std::unordered_map<uint64_t, physical_device_data> physical_devices;
        std::unordered_map<VkPhysicalDevice, uint64_t> physical_device_ids;
        struct queue_data
        {
            VkQueue handle{};
            uint64_t device_id{};
        };

        struct command_pool_data
        {
            VkCommandPool handle{};
            uint64_t device_id{};
        };

        struct command_buffer_data
        {
            VkCommandBuffer handle{};
            uint64_t device_id{};
            uint64_t pool_id{};
        };

        struct fence_data
        {
            VkFence handle{};
            uint64_t device_id{};
        };

        struct event_data
        {
            VkEvent handle{};
            uint64_t device_id{};
        };

        struct semaphore_data
        {
            VkSemaphore handle{};
            uint64_t device_id{};
        };

        struct memory_data
        {
            VkDeviceMemory handle{};
            uint64_t device_id{};
            void* persistent_host_pointer{};
            uint64_t mapped_size{};
            uint64_t allocation_size{};
        };

        struct buffer_data
        {
            VkBuffer handle{};
            uint64_t device_id{};
            uint64_t memory_id{};
            uint64_t memory_offset{};
            uint64_t size{};
        };

        struct image_data
        {
            VkImage handle{};
            uint64_t device_id{};
            uint32_t samples{1};
        };

        struct sampler_data
        {
            VkSampler handle{};
            uint64_t device_id{};
        };

        std::unordered_map<uint64_t, device_data> devices;
        std::unordered_map<uint64_t, queue_data> queues;
        std::unordered_map<uint64_t, command_pool_data> command_pools;
        std::unordered_map<uint64_t, command_buffer_data> command_buffers;
        std::unordered_map<uint64_t, fence_data> fences;
        std::unordered_map<uint64_t, event_data> events;
        std::unordered_map<uint64_t, semaphore_data> semaphores;
        std::unordered_map<uint64_t, memory_data> memories;
        std::unordered_map<uint64_t, buffer_data> buffers;
        std::unordered_map<uint64_t, image_data> images;
        std::unordered_map<uint64_t, sampler_data> samplers;
        std::unordered_map<uint64_t, surface_data> surfaces;
        std::unordered_map<uint64_t, swapchain_data> swapchains;

        // Graphics-pipeline objects. All non-dispatchable, device-owned, so one shape suffices.
        struct shader_module_data
        {
            VkShaderModule handle{};
            uint64_t device_id{};
        };
        struct image_view_data
        {
            VkImageView handle{};
            uint64_t device_id{};
        };
        struct buffer_view_data
        {
            VkBufferView handle{};
            uint64_t device_id{};
        };
        struct query_pool_data
        {
            VkQueryPool handle{};
            uint64_t device_id{};
        };
        struct render_pass_data
        {
            VkRenderPass handle{};
            uint64_t device_id{};
            bool has_depth{};
        };
        struct framebuffer_data
        {
            VkFramebuffer handle{};
            uint64_t device_id{};
        };
        struct pipeline_layout_data
        {
            VkPipelineLayout handle{};
            uint64_t device_id{};
        };
        struct pipeline_data
        {
            VkPipeline handle{};
            uint64_t device_id{};
        };
        struct descriptor_set_layout_data
        {
            VkDescriptorSetLayout handle{};
            uint64_t device_id{};
        };
        struct descriptor_pool_data
        {
            VkDescriptorPool handle{};
            uint64_t device_id{};
        };
        struct bound_buffer_info
        {
            uint64_t buffer_id{};
            uint64_t offset{};
            uint64_t range{};
            uint32_t type{};
        };

        struct descriptor_set_data
        {
            VkDescriptorSet handle{};
            uint64_t device_id{};
            uint64_t pool_id{};
            std::unordered_map<uint32_t, bound_buffer_info> buffer_bindings;
        };

        std::unordered_map<uint64_t, shader_module_data> shader_modules;
        std::unordered_map<uint64_t, image_view_data> image_views;
        std::unordered_map<uint64_t, buffer_view_data> buffer_views;
        std::unordered_map<uint64_t, query_pool_data> query_pools;
        std::unordered_map<uint64_t, render_pass_data> render_passes;
        std::unordered_map<uint64_t, framebuffer_data> framebuffers;
        std::unordered_map<uint64_t, pipeline_layout_data> pipeline_layouts;
        std::unordered_map<uint64_t, pipeline_data> pipelines;
        std::unordered_map<uint64_t, descriptor_set_layout_data> descriptor_set_layouts;
        std::unordered_map<uint64_t, descriptor_pool_data> descriptor_pools;
        std::unordered_map<uint64_t, descriptor_set_data> descriptor_sets;
        uint64_t next_id{1};

        // Picks the first memory type set in `type_bits` that has all `required` property flags, using
        // the memory properties of the device's physical device. Returns UINT32_MAX if none qualifies.
        uint32_t find_memory_type(const device_data& dev, uint32_t type_bits, VkMemoryPropertyFlags required)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_physical_device_memory_properties)
            {
                return UINT32_MAX;
            }

            VkPhysicalDeviceMemoryProperties props{};
            instance->second.get_physical_device_memory_properties(dev.physical_device, &props);

            for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            {
                if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
                {
                    return i;
                }
            }
            return UINT32_MAX;
        }

        template <typename Map, typename Pred>
        void erase_owned(Map& map, Pred pred)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                it = pred(it->second) ? map.erase(it) : std::next(it);
            }
        }

        // Frees every Vulkan resource a swapchain owns (offscreen images + memory, readback buffer +
        // memory, present command pool/fence) and removes its images from the `images` table.
        void destroy_swapchain_resources(swapchain_data& sc)
        {
            const auto dev_it = this->devices.find(sc.device_id);
            if (dev_it == this->devices.end())
            {
                return;
            }
            device_data& dev = dev_it->second;

            // A deferred present copy may still be in flight on the GPU; wait it out before tearing down
            // the command pool / fence / buffer it uses.
            if (sc.present_in_flight && dev.get_fence_status && dev.queue_wait_idle &&
                dev.get_fence_status(dev.handle, sc.present_fence) != VK_SUCCESS)
            {
                // No queue handle here, so flush the whole device instead.
                if (dev.device_wait_idle)
                {
                    dev.device_wait_idle(dev.handle);
                }
            }
            sc.present_in_flight = false;

            for (const uint64_t image_id : sc.image_ids)
            {
                const auto img = this->images.find(image_id);
                if (img != this->images.end())
                {
                    if (img->second.handle && dev.destroy_image)
                    {
                        dev.destroy_image(dev.handle, img->second.handle, nullptr);
                    }
                    this->images.erase(img);
                }
            }
            for (VkDeviceMemory memory : sc.image_memory)
            {
                if (memory && dev.free_memory)
                {
                    dev.free_memory(dev.handle, memory, nullptr);
                }
            }
            if (sc.present_fence && dev.destroy_fence)
            {
                dev.destroy_fence(dev.handle, sc.present_fence, nullptr);
            }
            if (sc.present_pool && dev.destroy_command_pool)
            {
                dev.destroy_command_pool(dev.handle, sc.present_pool, nullptr); // also frees present_cmd
            }
            if (sc.readback_buffer && dev.destroy_buffer)
            {
                dev.destroy_buffer(dev.handle, sc.readback_buffer, nullptr);
            }
            if (sc.readback_memory && dev.free_memory)
            {
                dev.free_memory(dev.handle, sc.readback_memory, nullptr);
            }
        }

        void erase_device(uint64_t device_id)
        {
            const auto it = this->devices.find(device_id);
            if (it == this->devices.end())
            {
                return;
            }

            // Tear down swapchains first: they own offscreen images (in the `images` table), a readback
            // buffer, and a present command pool/fence that must go before the device.
            for (auto sc = this->swapchains.begin(); sc != this->swapchains.end();)
            {
                if (sc->second.device_id == device_id)
                {
                    this->destroy_swapchain_resources(sc->second);
                    sc = this->swapchains.erase(sc);
                }
                else
                {
                    ++sc;
                }
            }

            // Descriptor pools (which free their sets) before descriptor set layouts those sets used.
            for (auto& [id, pool] : this->descriptor_pools)
            {
                if (pool.device_id == device_id && pool.handle && it->second.destroy_descriptor_pool)
                {
                    it->second.destroy_descriptor_pool(it->second.handle, pool.handle, nullptr);
                }
            }
            for (auto& [id, layout] : this->descriptor_set_layouts)
            {
                if (layout.device_id == device_id && layout.handle && it->second.destroy_descriptor_set_layout)
                {
                    it->second.destroy_descriptor_set_layout(it->second.handle, layout.handle, nullptr);
                }
            }
            this->erase_owned(this->descriptor_sets, [&](const descriptor_set_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->descriptor_pools, [&](const descriptor_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->descriptor_set_layouts, [&](const descriptor_set_layout_data& d) { return d.device_id == device_id; });

            // Graphics-pipeline objects, in dependency order (framebuffers/pipelines reference render
            // passes, layouts, shader modules and image views; image views reference images).
            for (auto& [id, fb] : this->framebuffers)
            {
                if (fb.device_id == device_id && fb.handle && it->second.destroy_framebuffer)
                {
                    it->second.destroy_framebuffer(it->second.handle, fb.handle, nullptr);
                }
            }
            for (auto& [id, pipe] : this->pipelines)
            {
                if (pipe.device_id == device_id && pipe.handle && it->second.destroy_pipeline)
                {
                    it->second.destroy_pipeline(it->second.handle, pipe.handle, nullptr);
                }
            }
            for (auto& [id, layout] : this->pipeline_layouts)
            {
                if (layout.device_id == device_id && layout.handle && it->second.destroy_pipeline_layout)
                {
                    it->second.destroy_pipeline_layout(it->second.handle, layout.handle, nullptr);
                }
            }
            for (auto& [id, rp] : this->render_passes)
            {
                if (rp.device_id == device_id && rp.handle && it->second.destroy_render_pass)
                {
                    it->second.destroy_render_pass(it->second.handle, rp.handle, nullptr);
                }
            }
            for (auto& [id, view] : this->image_views)
            {
                if (view.device_id == device_id && view.handle && it->second.destroy_image_view)
                {
                    it->second.destroy_image_view(it->second.handle, view.handle, nullptr);
                }
            }
            for (auto& [id, view] : this->buffer_views)
            {
                if (view.device_id == device_id && view.handle && it->second.destroy_buffer_view)
                {
                    it->second.destroy_buffer_view(it->second.handle, view.handle, nullptr);
                }
            }
            for (auto& [id, qp] : this->query_pools)
            {
                if (qp.device_id == device_id && qp.handle && it->second.destroy_query_pool)
                {
                    it->second.destroy_query_pool(it->second.handle, qp.handle, nullptr);
                }
            }
            for (auto& [id, sm] : this->shader_modules)
            {
                if (sm.device_id == device_id && sm.handle && it->second.destroy_shader_module)
                {
                    it->second.destroy_shader_module(it->second.handle, sm.handle, nullptr);
                }
            }
            this->erase_owned(this->framebuffers, [&](const framebuffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->pipelines, [&](const pipeline_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->pipeline_layouts, [&](const pipeline_layout_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->render_passes, [&](const render_pass_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->image_views, [&](const image_view_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->buffer_views, [&](const buffer_view_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->query_pools, [&](const query_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->shader_modules, [&](const shader_module_data& d) { return d.device_id == device_id; });

            // Destroy GPU-owned children (pools free their command buffers; fences are freed) before
            // the device itself.
            for (auto& [id, pool] : this->command_pools)
            {
                if (pool.device_id == device_id && pool.handle && it->second.destroy_command_pool)
                {
                    it->second.destroy_command_pool(it->second.handle, pool.handle, nullptr);
                }
            }
            for (auto& [id, fence] : this->fences)
            {
                if (fence.device_id == device_id && fence.handle && it->second.destroy_fence)
                {
                    it->second.destroy_fence(it->second.handle, fence.handle, nullptr);
                }
            }
            for (auto& [id, event] : this->events)
            {
                if (event.device_id == device_id && event.handle && it->second.destroy_event)
                {
                    it->second.destroy_event(it->second.handle, event.handle, nullptr);
                }
            }
            for (auto& [id, semaphore] : this->semaphores)
            {
                if (semaphore.device_id == device_id && semaphore.handle && it->second.destroy_semaphore)
                {
                    it->second.destroy_semaphore(it->second.handle, semaphore.handle, nullptr);
                }
            }
            // Buffers and images must be destroyed before the memory they are bound to is freed.
            for (auto& [id, buffer] : this->buffers)
            {
                if (buffer.device_id == device_id && buffer.handle && it->second.destroy_buffer)
                {
                    it->second.destroy_buffer(it->second.handle, buffer.handle, nullptr);
                }
            }
            for (auto& [id, image] : this->images)
            {
                if (image.device_id == device_id && image.handle && it->second.destroy_image)
                {
                    it->second.destroy_image(it->second.handle, image.handle, nullptr);
                }
            }
            for (auto& [id, sampler] : this->samplers)
            {
                if (sampler.device_id == device_id && sampler.handle && it->second.destroy_sampler)
                {
                    it->second.destroy_sampler(it->second.handle, sampler.handle, nullptr);
                }
            }
            for (auto& [id, memory] : this->memories)
            {
                if (memory.device_id == device_id && memory.handle && it->second.free_memory)
                {
                    it->second.free_memory(it->second.handle, memory.handle, nullptr);
                }
            }

            this->erase_owned(this->command_buffers, [&](const command_buffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->command_pools, [&](const command_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->fences, [&](const fence_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->events, [&](const event_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->semaphores, [&](const semaphore_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->buffers, [&](const buffer_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->images, [&](const image_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->samplers, [&](const sampler_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->memories, [&](const memory_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->queues, [&](const queue_data& d) { return d.device_id == device_id; });

            if (it->second.handle && it->second.destroy_device)
            {
                it->second.destroy_device(it->second.handle, nullptr);
            }

            this->devices.erase(it);
        }

        impl()
        {
            if constexpr (sizeof(size_t) != 8)
            {
                return;
            }

            for (const auto* name : vulkan_loader_names)
            {
                this->loader = load_library(name);
                if (this->loader)
                {
                    break;
                }
            }

            if (!this->loader)
            {
                return;
            }

            this->get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_symbol(this->loader, "vkGetInstanceProcAddr"));
            if (!this->get_instance_proc_addr)
            {
                return;
            }

            this->create_instance = reinterpret_cast<PFN_vkCreateInstance>(this->get_instance_proc_addr(nullptr, "vkCreateInstance"));
            this->enumerate_instance_version =
                reinterpret_cast<PFN_vkEnumerateInstanceVersion>(this->get_instance_proc_addr(nullptr, "vkEnumerateInstanceVersion"));
        }

        ~impl()
        {
            for (auto& [id, device] : this->devices)
            {
                if (device.handle && device.destroy_device)
                {
                    device.destroy_device(device.handle, nullptr);
                }
            }

            for (auto& [id, instance] : this->instances)
            {
                if (instance.handle && instance.destroy_instance)
                {
                    instance.destroy_instance(instance.handle, nullptr);
                }
            }

            if (this->loader)
            {
                free_library(this->loader);
            }
        }

        template <typename Fn>
        Fn load_instance_proc(VkInstance instance, const char* name) const
        {
            return reinterpret_cast<Fn>(this->get_instance_proc_addr(instance, name));
        }
    };

    vulkan_host::vulkan_host()
        : impl_(std::make_unique<impl>())
    {
    }

    vulkan_host::~vulkan_host() = default;

    bool vulkan_host::available() const
    {
        return this->impl_->create_instance != nullptr;
    }

    int32_t vulkan_host::create_instance(uint64_t& out_instance)
    {
        out_instance = 0;

        if (!this->available())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t api_version = VK_API_VERSION_1_0;
        if (this->impl_->enumerate_instance_version)
        {
            uint32_t supported_version = VK_API_VERSION_1_0;
            if (this->impl_->enumerate_instance_version(&supported_version) == VK_SUCCESS)
            {
                api_version = std::min(supported_version, static_cast<uint32_t>(VK_API_VERSION_1_3));
            }
        }

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.apiVersion = api_version;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        VkInstance instance{};
        const VkResult result = this->impl_->create_instance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        impl::instance_data data{};
        data.handle = instance;
        data.destroy_instance = this->impl_->load_instance_proc<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
        data.enumerate_physical_devices =
            this->impl_->load_instance_proc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
        data.get_physical_device_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
        data.get_physical_device_format_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceFormatProperties>(instance, "vkGetPhysicalDeviceFormatProperties");
        data.get_physical_device_image_format_properties = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceImageFormatProperties>(
            instance, "vkGetPhysicalDeviceImageFormatProperties");
        data.get_queue_family_properties = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        data.get_physical_device_memory_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
        data.get_physical_device_features2 =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
        data.get_physical_device_properties2 =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
        data.enumerate_device_extension_properties =
            this->impl_->load_instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
        data.create_device = this->impl_->load_instance_proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
        data.get_device_proc_addr = this->impl_->load_instance_proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");

        const uint64_t id = this->impl_->next_id++;
        this->impl_->instances.emplace(id, data);
        out_instance = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_instance(uint64_t instance)
    {
        const auto it = this->impl_->instances.find(instance);
        if (it == this->impl_->instances.end())
        {
            return;
        }

        // Tear down logical devices (and their queues) created from this instance first.
        std::vector<uint64_t> owned_devices;
        for (const auto& [device_id, device] : this->impl_->devices)
        {
            if (device.instance_id == instance)
            {
                owned_devices.push_back(device_id);
            }
        }
        for (const uint64_t device_id : owned_devices)
        {
            this->impl_->erase_device(device_id);
        }

        if (it->second.handle && it->second.destroy_instance)
        {
            it->second.destroy_instance(it->second.handle, nullptr);
        }

        // Drop physical devices that belonged to this instance.
        for (auto pd = this->impl_->physical_devices.begin(); pd != this->impl_->physical_devices.end();)
        {
            if (pd->second.instance_id == instance)
            {
                this->impl_->physical_device_ids.erase(pd->second.handle);
                pd = this->impl_->physical_devices.erase(pd);
            }
            else
            {
                ++pd;
            }
        }

        this->impl_->instances.erase(it);
    }

    int32_t vulkan_host::enumerate_physical_devices(uint64_t instance, std::span<uint64_t> out_devices, uint32_t& out_count)
    {
        out_count = 0;

        const auto it = this->impl_->instances.find(instance);
        if (it == this->impl_->instances.end() || !it->second.enumerate_physical_devices)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t count = 0;
        VkResult result = it->second.enumerate_physical_devices(it->second.handle, &count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return result;
        }

        std::vector<VkPhysicalDevice> devices(count);
        if (count > 0)
        {
            result = it->second.enumerate_physical_devices(it->second.handle, &count, devices.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return result;
            }
        }

        out_count = count;

        for (uint32_t i = 0; i < count && i < out_devices.size(); ++i)
        {
            const VkPhysicalDevice device = devices[i];

            auto id_it = this->impl_->physical_device_ids.find(device);
            if (id_it == this->impl_->physical_device_ids.end())
            {
                const uint64_t id = this->impl_->next_id++;
                this->impl_->physical_devices.emplace(id, impl::physical_device_data{.handle = device, .instance_id = instance});
                id_it = this->impl_->physical_device_ids.emplace(device, id).first;
            }

            out_devices[i] = id_it->second;
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_properties(uint64_t physical_device, void* out, size_t out_size, bool guest_is_32bit)
    {
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceProperties properties{};
        instance->second.get_physical_device_properties(pd->second.handle, &properties);

        if (guest_is_32bit)
        {
            repack_properties_for_wow64(properties, out, out_size);
        }
        else
        {
            std::memcpy(out, &properties, std::min(out_size, sizeof(properties)));
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_format_properties(uint64_t physical_device, uint32_t format, uint32_t& out_linear,
                                                               uint32_t& out_optimal, uint32_t& out_buffer)
    {
        out_linear = 0;
        out_optimal = 0;
        out_buffer = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_format_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFormatProperties properties{};
        instance->second.get_physical_device_format_properties(pd->second.handle, static_cast<VkFormat>(format), &properties);
        out_linear = properties.linearTilingFeatures;
        out_optimal = properties.optimalTilingFeatures;
        out_buffer = properties.bufferFeatures;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_image_format_properties(uint64_t physical_device, uint32_t format, uint32_t type,
                                                                     uint32_t tiling, uint32_t usage, uint32_t flags,
                                                                     image_format_properties& out)
    {
        out = {};

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_image_format_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageFormatProperties properties{};
        const VkResult result = instance->second.get_physical_device_image_format_properties(
            pd->second.handle, static_cast<VkFormat>(format), static_cast<VkImageType>(type), static_cast<VkImageTiling>(tiling), usage,
            flags, &properties);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        out.max_mip_levels = properties.maxMipLevels;
        out.max_array_layers = properties.maxArrayLayers;
        out.sample_counts = properties.sampleCounts;
        out.max_extent_width = properties.maxExtent.width;
        out.max_extent_height = properties.maxExtent.height;
        out.max_extent_depth = properties.maxExtent.depth;
        out.max_resource_size = properties.maxResourceSize;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_queue_family_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count)
    {
        out_count = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_queue_family_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t count = 0;
        instance->second.get_queue_family_properties(pd->second.handle, &count, nullptr);

        std::vector<VkQueueFamilyProperties> families(count);
        if (count > 0)
        {
            instance->second.get_queue_family_properties(pd->second.handle, &count, families.data());
        }

        out_count = count;

        const size_t copy_bytes = std::min(out_size, families.size() * sizeof(VkQueueFamilyProperties));
        if (copy_bytes > 0)
        {
            std::memcpy(out, families.data(), copy_bytes);
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::enumerate_device_extension_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count)
    {
        out_count = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.enumerate_device_extension_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint32_t count = 0;
        VkResult result = instance->second.enumerate_device_extension_properties(pd->second.handle, nullptr, &count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return result;
        }

        std::vector<VkExtensionProperties> extensions(count);
        if (count > 0)
        {
            result = instance->second.enumerate_device_extension_properties(pd->second.handle, nullptr, &count, extensions.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return result;
            }
        }

        out_count = count;

        const size_t copy_bytes = std::min(out_size, extensions.size() * sizeof(VkExtensionProperties));
        if (copy_bytes > 0)
        {
            std::memcpy(out, extensions.data(), copy_bytes);
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_features2(uint64_t physical_device, const void* in_records, size_t in_records_size,
                                                       uint32_t struct_count, std::vector<std::byte>& out_blob)
    {
        out_blob.clear();

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_features2)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (in_records_size < static_cast<size_t>(struct_count) * sizeof(gpu_bridge::feature_chain_record))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto* records = static_cast<const gpu_bridge::feature_chain_record*>(in_records);

        // Build a VkPhysicalDeviceFeatures2 with one allocated, zeroed struct per known requested sType
        // chained on pNext. The root features2 carries the base VkPhysicalDeviceFeatures.
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        std::vector<std::vector<std::byte>> chained; // owns each pNext struct's bytes
        auto* tail = reinterpret_cast<VkBaseOutStructure*>(&features2);
        for (uint32_t i = 0; i < struct_count; ++i)
        {
            const auto type = static_cast<VkStructureType>(records[i].s_type);
            if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            {
                continue; // the root struct itself
            }
            const size_t size = gpu_bridge::feature_struct_size(type);
            if (size == 0)
            {
                continue; // unknown struct: not queried, reported empty below
            }
            auto& buffer = chained.emplace_back(size, std::byte{});
            auto* base = reinterpret_cast<VkBaseOutStructure*>(buffer.data());
            base->sType = type;
            base->pNext = nullptr;
            tail->pNext = base;
            tail = base;
        }

        instance->second.get_physical_device_features2(pd->second.handle, &features2);

        // Serialize one record + body per requested struct, in request order. The body is the guest's
        // pad-free VkBool32 run copied from after the (ABI-specific) header.
        for (uint32_t i = 0; i < struct_count; ++i)
        {
            const auto type = static_cast<VkStructureType>(records[i].s_type);

            const std::byte* body = nullptr;
            if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            {
                body = reinterpret_cast<const std::byte*>(&features2) + gpu_bridge::feature_chain_header_size;
            }
            else
            {
                for (const auto& buffer : chained)
                {
                    if (reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType == type)
                    {
                        body = buffer.data() + gpu_bridge::feature_chain_header_size;
                        break;
                    }
                }
            }

            uint32_t body_size = 0;
            if (body)
            {
                const size_t host_capacity = gpu_bridge::feature_struct_size(type) - gpu_bridge::feature_chain_header_size;
                body_size = static_cast<uint32_t>(std::min<size_t>(records[i].body_size, host_capacity));
            }

            const gpu_bridge::feature_chain_record out_record{.s_type = records[i].s_type, .body_size = body_size};
            const auto* record_bytes = reinterpret_cast<const std::byte*>(&out_record);
            out_blob.insert(out_blob.end(), record_bytes, record_bytes + sizeof(out_record));
            if (body_size > 0)
            {
                out_blob.insert(out_blob.end(), body, body + body_size);
            }
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_properties2(uint64_t physical_device, const void* in_records, size_t in_records_size,
                                                         uint32_t struct_count, std::vector<std::byte>& out_blob)
    {
        out_blob.clear();

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_properties2)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (in_records_size < static_cast<size_t>(struct_count) * sizeof(gpu_bridge::feature_chain_record))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto* records = static_cast<const gpu_bridge::feature_chain_record*>(in_records);

        // Build a VkPhysicalDeviceProperties2 with one allocated, zeroed struct per known requested sType
        // chained on pNext. Only the chained property structs are serialized back; the base properties go
        // through get_physical_device_properties.
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

        std::vector<std::vector<std::byte>> chained;
        auto* tail = reinterpret_cast<VkBaseOutStructure*>(&properties2);
        for (uint32_t i = 0; i < struct_count; ++i)
        {
            const auto type = static_cast<VkStructureType>(records[i].s_type);
            const size_t size = gpu_bridge::property_struct_size(type);
            if (size == 0)
            {
                continue; // unknown struct: not queried, reported empty below
            }
            auto& buffer = chained.emplace_back(size, std::byte{});
            auto* base = reinterpret_cast<VkBaseOutStructure*>(buffer.data());
            base->sType = type;
            base->pNext = nullptr;
            tail->pNext = base;
            tail = base;
        }

        instance->second.get_physical_device_properties2(pd->second.handle, &properties2);

        for (uint32_t i = 0; i < struct_count; ++i)
        {
            const auto type = static_cast<VkStructureType>(records[i].s_type);

            const std::byte* body = nullptr;
            for (const auto& buffer : chained)
            {
                if (reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType == type)
                {
                    body = buffer.data() + gpu_bridge::feature_chain_header_size;
                    break;
                }
            }

            uint32_t body_size = 0;
            if (body)
            {
                const size_t host_capacity = gpu_bridge::property_struct_size(type) - gpu_bridge::feature_chain_header_size;
                body_size = static_cast<uint32_t>(std::min<size_t>(records[i].body_size, host_capacity));
            }

            const gpu_bridge::feature_chain_record out_record{.s_type = records[i].s_type, .body_size = body_size};
            const auto* record_bytes = reinterpret_cast<const std::byte*>(&out_record);
            out_blob.insert(out_blob.end(), record_bytes, record_bytes + sizeof(out_record));
            if (body_size > 0)
            {
                out_blob.insert(out_blob.end(), body, body + body_size);
            }
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_device(uint64_t physical_device, const void* queue_entries, size_t queue_entry_count,
                                       const void* extension_blob, size_t extension_blob_size, uint32_t extension_count,
                                       const void* feature_blob, size_t feature_blob_size, uint32_t feature_struct_count,
                                       uint64_t& out_device)
    {
        out_device = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.create_device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Build one VkDeviceQueueCreateInfo per requested family so vkGetDeviceQueue later succeeds for
        // each (DXVK retrieves a graphics queue plus a separate transfer/compute family). Each family's
        // priorities array must outlive the create call, so the priority buffers are kept alongside.
        const auto* entries = static_cast<const gpu_bridge::device_queue_create_entry*>(queue_entries);
        const size_t entry_count = (queue_entry_count == 0) ? 1 : queue_entry_count;

        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        std::vector<std::vector<float>> priorities_store;
        queue_infos.reserve(entry_count);
        priorities_store.reserve(entry_count);
        for (size_t i = 0; i < entry_count; ++i)
        {
            const uint32_t family = entries ? entries[i].queue_family_index : 0;
            const uint32_t requested = entries ? entries[i].queue_count : 1;
            const uint32_t queues = (requested == 0) ? 1 : requested;

            auto& priorities = priorities_store.emplace_back(queues, 1.0f);
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = queues;
            queue_info.pQueuePriorities = priorities.data();
            queue_infos.push_back(queue_info);
        }
        const uint32_t primary_family = queue_infos.front().queueFamilyIndex;

        // Rebuild the enabled-extension name list from the blob (count NUL-terminated strings).
        std::vector<const char*> extensions;
        extensions.reserve(extension_count);
        {
            const auto* cursor = static_cast<const char*>(extension_blob);
            const char* const end = cursor + extension_blob_size;
            for (uint32_t i = 0; i < extension_count && cursor < end; ++i)
            {
                const auto* terminator = static_cast<const char*>(std::memchr(cursor, '\0', static_cast<size_t>(end - cursor)));
                if (!terminator)
                {
                    break;
                }
                extensions.push_back(cursor);
                cursor = terminator + 1;
            }
        }

        // Rebuild the pNext feature chain to enable (same record format as get_physical_device_features2);
        // the VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 record carries the base VkPhysicalDeviceFeatures.
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        std::vector<std::vector<std::byte>> chained;
        auto* feature_tail = reinterpret_cast<VkBaseOutStructure*>(&features2);
        bool has_features = false;
        {
            const auto* cursor = static_cast<const std::byte*>(feature_blob);
            const std::byte* const end = cursor + feature_blob_size;
            for (uint32_t i = 0; i < feature_struct_count; ++i)
            {
                if (cursor + sizeof(gpu_bridge::feature_chain_record) > end)
                {
                    break;
                }
                gpu_bridge::feature_chain_record record{};
                std::memcpy(&record, cursor, sizeof(record));
                cursor += sizeof(record);
                if (cursor + record.body_size > end)
                {
                    break;
                }

                const auto type = static_cast<VkStructureType>(record.s_type);
                const size_t size = gpu_bridge::feature_struct_size(type);
                if (size != 0)
                {
                    const size_t capacity = size - gpu_bridge::feature_chain_header_size;
                    const size_t copy = std::min<size_t>(record.body_size, capacity);
                    if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                    {
                        has_features = true;
                        std::memcpy(reinterpret_cast<std::byte*>(&features2) + gpu_bridge::feature_chain_header_size, cursor, copy);
                    }
                    else
                    {
                        auto& buffer = chained.emplace_back(size, std::byte{});
                        auto* base = reinterpret_cast<VkBaseOutStructure*>(buffer.data());
                        base->sType = type;
                        base->pNext = nullptr;
                        std::memcpy(buffer.data() + gpu_bridge::feature_chain_header_size, cursor, copy);
                        feature_tail->pNext = base;
                        feature_tail = base;
                    }
                }
                cursor += record.body_size;
            }
        }

        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        create_info.pQueueCreateInfos = queue_infos.data();
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
        // Enabled features ride the pNext chain (VkPhysicalDeviceFeatures2 + the chained structs); a
        // chain present means pEnabledFeatures must stay null.
        if (has_features || feature_tail != reinterpret_cast<VkBaseOutStructure*>(&features2))
        {
            create_info.pNext = &features2;
        }

        VkDevice device{};
        const VkResult result = instance->second.create_device(pd->second.handle, &create_info, nullptr, &device);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        impl::device_data data{};
        data.handle = device;
        data.instance_id = pd->second.instance_id;
        data.physical_device = pd->second.handle;
        data.queue_family_index = primary_family;

        if (const auto gdpa = instance->second.get_device_proc_addr)
        {
            const auto resolve = [&](const char* name) { return gdpa(device, name); };

            data.destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(resolve("vkDestroyDevice"));
            data.get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve("vkGetDeviceQueue"));
            data.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(resolve("vkQueueWaitIdle"));
            data.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(resolve("vkDeviceWaitIdle"));
            data.create_command_pool = reinterpret_cast<PFN_vkCreateCommandPool>(resolve("vkCreateCommandPool"));
            data.destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(resolve("vkDestroyCommandPool"));
            data.allocate_command_buffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(resolve("vkAllocateCommandBuffers"));
            data.free_command_buffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(resolve("vkFreeCommandBuffers"));
            data.begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(resolve("vkBeginCommandBuffer"));
            data.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(resolve("vkEndCommandBuffer"));
            data.reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(resolve("vkResetCommandPool"));
            data.reset_command_buffer = reinterpret_cast<PFN_vkResetCommandBuffer>(resolve("vkResetCommandBuffer"));
            data.create_fence = reinterpret_cast<PFN_vkCreateFence>(resolve("vkCreateFence"));
            data.destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(resolve("vkDestroyFence"));
            data.reset_fences = reinterpret_cast<PFN_vkResetFences>(resolve("vkResetFences"));
            data.create_event = reinterpret_cast<PFN_vkCreateEvent>(resolve("vkCreateEvent"));
            data.destroy_event = reinterpret_cast<PFN_vkDestroyEvent>(resolve("vkDestroyEvent"));
            data.get_event_status = reinterpret_cast<PFN_vkGetEventStatus>(resolve("vkGetEventStatus"));
            data.set_event = reinterpret_cast<PFN_vkSetEvent>(resolve("vkSetEvent"));
            data.reset_event = reinterpret_cast<PFN_vkResetEvent>(resolve("vkResetEvent"));
            data.create_semaphore = reinterpret_cast<PFN_vkCreateSemaphore>(resolve("vkCreateSemaphore"));
            data.destroy_semaphore = reinterpret_cast<PFN_vkDestroySemaphore>(resolve("vkDestroySemaphore"));
            data.get_semaphore_counter_value = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(resolve("vkGetSemaphoreCounterValue"));
            data.signal_semaphore = reinterpret_cast<PFN_vkSignalSemaphore>(resolve("vkSignalSemaphore"));
            data.wait_semaphores = reinterpret_cast<PFN_vkWaitSemaphores>(resolve("vkWaitSemaphores"));
            data.get_buffer_device_address = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(resolve("vkGetBufferDeviceAddress"));
            data.get_fence_status = reinterpret_cast<PFN_vkGetFenceStatus>(resolve("vkGetFenceStatus"));
            data.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(resolve("vkQueueSubmit"));
            data.queue_submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(resolve("vkQueueSubmit2"));
            data.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(resolve("vkAllocateMemory"));
            data.free_memory = reinterpret_cast<PFN_vkFreeMemory>(resolve("vkFreeMemory"));
            data.map_memory = reinterpret_cast<PFN_vkMapMemory>(resolve("vkMapMemory"));
            data.unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(resolve("vkUnmapMemory"));
            data.flush_mapped_memory_ranges = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(resolve("vkFlushMappedMemoryRanges"));
            data.invalidate_mapped_memory_ranges =
                reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(resolve("vkInvalidateMappedMemoryRanges"));
            data.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(resolve("vkCreateBuffer"));
            data.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(resolve("vkDestroyBuffer"));
            data.get_buffer_memory_requirements =
                reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(resolve("vkGetBufferMemoryRequirements"));
            data.bind_buffer_memory = reinterpret_cast<PFN_vkBindBufferMemory>(resolve("vkBindBufferMemory"));
            data.cmd_fill_buffer = reinterpret_cast<PFN_vkCmdFillBuffer>(resolve("vkCmdFillBuffer"));
            data.create_image = reinterpret_cast<PFN_vkCreateImage>(resolve("vkCreateImage"));
            data.destroy_image = reinterpret_cast<PFN_vkDestroyImage>(resolve("vkDestroyImage"));
            data.get_image_memory_requirements =
                reinterpret_cast<PFN_vkGetImageMemoryRequirements>(resolve("vkGetImageMemoryRequirements"));
            data.get_image_subresource_layout = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(resolve("vkGetImageSubresourceLayout"));
            data.bind_image_memory = reinterpret_cast<PFN_vkBindImageMemory>(resolve("vkBindImageMemory"));
            data.cmd_pipeline_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(resolve("vkCmdPipelineBarrier"));
            data.cmd_clear_color_image = reinterpret_cast<PFN_vkCmdClearColorImage>(resolve("vkCmdClearColorImage"));
            data.cmd_clear_depth_stencil_image = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(resolve("vkCmdClearDepthStencilImage"));
            data.cmd_copy_image_to_buffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(resolve("vkCmdCopyImageToBuffer"));
            data.cmd_resolve_image = reinterpret_cast<PFN_vkCmdResolveImage>(resolve("vkCmdResolveImage"));
            data.cmd_update_buffer = reinterpret_cast<PFN_vkCmdUpdateBuffer>(resolve("vkCmdUpdateBuffer"));
            data.cmd_copy_buffer_to_image = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(resolve("vkCmdCopyBufferToImage"));
            data.cmd_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(resolve("vkCmdCopyImage"));
            data.cmd_copy_buffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(resolve("vkCmdCopyBuffer"));
            data.create_sampler = reinterpret_cast<PFN_vkCreateSampler>(resolve("vkCreateSampler"));
            data.destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(resolve("vkDestroySampler"));
            data.create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(resolve("vkCreateShaderModule"));
            data.destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(resolve("vkDestroyShaderModule"));
            data.create_image_view = reinterpret_cast<PFN_vkCreateImageView>(resolve("vkCreateImageView"));
            data.destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(resolve("vkDestroyImageView"));
            data.create_buffer_view = reinterpret_cast<PFN_vkCreateBufferView>(resolve("vkCreateBufferView"));
            data.destroy_buffer_view = reinterpret_cast<PFN_vkDestroyBufferView>(resolve("vkDestroyBufferView"));
            data.create_query_pool = reinterpret_cast<PFN_vkCreateQueryPool>(resolve("vkCreateQueryPool"));
            data.destroy_query_pool = reinterpret_cast<PFN_vkDestroyQueryPool>(resolve("vkDestroyQueryPool"));
            data.reset_query_pool = reinterpret_cast<PFN_vkResetQueryPool>(resolve("vkResetQueryPool"));
            data.get_query_pool_results = reinterpret_cast<PFN_vkGetQueryPoolResults>(resolve("vkGetQueryPoolResults"));
            data.cmd_reset_query_pool = reinterpret_cast<PFN_vkCmdResetQueryPool>(resolve("vkCmdResetQueryPool"));
            data.cmd_begin_query = reinterpret_cast<PFN_vkCmdBeginQuery>(resolve("vkCmdBeginQuery"));
            data.cmd_end_query = reinterpret_cast<PFN_vkCmdEndQuery>(resolve("vkCmdEndQuery"));
            data.cmd_write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(resolve("vkCmdWriteTimestamp"));
            data.create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(resolve("vkCreateRenderPass"));
            data.destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(resolve("vkDestroyRenderPass"));
            data.create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(resolve("vkCreateFramebuffer"));
            data.destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(resolve("vkDestroyFramebuffer"));
            data.create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(resolve("vkCreatePipelineLayout"));
            data.destroy_pipeline_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(resolve("vkDestroyPipelineLayout"));
            data.create_descriptor_set_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(resolve("vkCreateDescriptorSetLayout"));
            data.destroy_descriptor_set_layout =
                reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(resolve("vkDestroyDescriptorSetLayout"));
            data.create_descriptor_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(resolve("vkCreateDescriptorPool"));
            data.destroy_descriptor_pool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(resolve("vkDestroyDescriptorPool"));
            data.allocate_descriptor_sets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(resolve("vkAllocateDescriptorSets"));
            data.update_descriptor_sets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(resolve("vkUpdateDescriptorSets"));
            data.cmd_bind_descriptor_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(resolve("vkCmdBindDescriptorSets"));
            data.create_graphics_pipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(resolve("vkCreateGraphicsPipelines"));
            data.create_compute_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(resolve("vkCreateComputePipelines"));
            data.destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(resolve("vkDestroyPipeline"));
            data.cmd_begin_render_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(resolve("vkCmdBeginRenderPass"));
            data.cmd_begin_rendering = reinterpret_cast<PFN_vkCmdBeginRendering>(resolve("vkCmdBeginRendering"));
            data.cmd_end_rendering = reinterpret_cast<PFN_vkCmdEndRendering>(resolve("vkCmdEndRendering"));
            data.cmd_execute_commands = reinterpret_cast<PFN_vkCmdExecuteCommands>(resolve("vkCmdExecuteCommands"));
            data.cmd_bind_pipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(resolve("vkCmdBindPipeline"));
            data.cmd_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(resolve("vkCmdDispatch"));
            data.cmd_dispatch_indirect = reinterpret_cast<PFN_vkCmdDispatchIndirect>(resolve("vkCmdDispatchIndirect"));
            data.cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(resolve("vkCmdDraw"));
            data.cmd_bind_vertex_buffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(resolve("vkCmdBindVertexBuffers"));
            data.cmd_bind_vertex_buffers2 = reinterpret_cast<PFN_vkCmdBindVertexBuffers2>(resolve("vkCmdBindVertexBuffers2"));
            data.cmd_bind_index_buffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(resolve("vkCmdBindIndexBuffer"));
            data.cmd_draw_indexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(resolve("vkCmdDrawIndexed"));
            data.cmd_end_render_pass = reinterpret_cast<PFN_vkCmdEndRenderPass>(resolve("vkCmdEndRenderPass"));
            data.cmd_push_constants = reinterpret_cast<PFN_vkCmdPushConstants>(resolve("vkCmdPushConstants"));
            data.cmd_set_viewport = reinterpret_cast<PFN_vkCmdSetViewport>(resolve("vkCmdSetViewport"));
            data.cmd_set_viewport_with_count = reinterpret_cast<PFN_vkCmdSetViewportWithCount>(resolve("vkCmdSetViewportWithCount"));
            data.cmd_set_scissor = reinterpret_cast<PFN_vkCmdSetScissor>(resolve("vkCmdSetScissor"));
            data.cmd_set_scissor_with_count = reinterpret_cast<PFN_vkCmdSetScissorWithCount>(resolve("vkCmdSetScissorWithCount"));
            data.cmd_set_depth_bias = reinterpret_cast<PFN_vkCmdSetDepthBias>(resolve("vkCmdSetDepthBias"));
            data.cmd_set_blend_constants = reinterpret_cast<PFN_vkCmdSetBlendConstants>(resolve("vkCmdSetBlendConstants"));
            data.cmd_set_depth_bounds = reinterpret_cast<PFN_vkCmdSetDepthBounds>(resolve("vkCmdSetDepthBounds"));
            data.cmd_set_line_width = reinterpret_cast<PFN_vkCmdSetLineWidth>(resolve("vkCmdSetLineWidth"));
            data.cmd_set_stencil_compare_mask = reinterpret_cast<PFN_vkCmdSetStencilCompareMask>(resolve("vkCmdSetStencilCompareMask"));
            data.cmd_set_stencil_write_mask = reinterpret_cast<PFN_vkCmdSetStencilWriteMask>(resolve("vkCmdSetStencilWriteMask"));
            data.cmd_set_stencil_reference = reinterpret_cast<PFN_vkCmdSetStencilReference>(resolve("vkCmdSetStencilReference"));
            data.cmd_set_stencil_op = reinterpret_cast<PFN_vkCmdSetStencilOp>(resolve("vkCmdSetStencilOp"));
            data.cmd_set_cull_mode = reinterpret_cast<PFN_vkCmdSetCullMode>(resolve("vkCmdSetCullMode"));
            data.cmd_set_front_face = reinterpret_cast<PFN_vkCmdSetFrontFace>(resolve("vkCmdSetFrontFace"));
            data.cmd_set_primitive_topology = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(resolve("vkCmdSetPrimitiveTopology"));
            data.cmd_set_depth_test_enable = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(resolve("vkCmdSetDepthTestEnable"));
            data.cmd_set_depth_write_enable = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(resolve("vkCmdSetDepthWriteEnable"));
            data.cmd_set_depth_compare_op = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(resolve("vkCmdSetDepthCompareOp"));
            data.cmd_set_depth_bounds_test_enable =
                reinterpret_cast<PFN_vkCmdSetDepthBoundsTestEnable>(resolve("vkCmdSetDepthBoundsTestEnable"));
            data.cmd_set_stencil_test_enable = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(resolve("vkCmdSetStencilTestEnable"));
            data.cmd_set_rasterizer_discard_enable =
                reinterpret_cast<PFN_vkCmdSetRasterizerDiscardEnable>(resolve("vkCmdSetRasterizerDiscardEnable"));
            data.cmd_set_depth_bias_enable = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(resolve("vkCmdSetDepthBiasEnable"));
            data.cmd_set_primitive_restart_enable =
                reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(resolve("vkCmdSetPrimitiveRestartEnable"));
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->devices.emplace(id, data);
        out_device = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_device(uint64_t device)
    {
        this->impl_->erase_device(device);
    }

    int32_t vulkan_host::get_device_queue(uint64_t device, uint32_t queue_family_index, uint32_t queue_index, uint64_t& out_queue)
    {
        out_queue = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.get_device_queue)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkQueue queue{};
        dev->second.get_device_queue(dev->second.handle, queue_family_index, queue_index, &queue);
        if (!queue)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->queues.emplace(id, impl::queue_data{.handle = queue, .device_id = device});
        out_queue = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_command_pool(uint64_t device, uint32_t queue_family_index, uint32_t flags, uint64_t& out_pool)
    {
        out_pool = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_command_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = flags;
        info.queueFamilyIndex = queue_family_index;

        VkCommandPool pool{};
        const VkResult result = dev->second.create_command_pool(dev->second.handle, &info, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->command_pools.emplace(id, impl::command_pool_data{.handle = pool, .device_id = device});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_command_pool(uint64_t device, uint64_t pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->command_pools.find(pool);
        if (it == this->impl_->command_pools.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_command_pool)
        {
            dev->second.destroy_command_pool(dev->second.handle, it->second.handle, nullptr);
        }

        // Command buffers from this pool are freed implicitly by the driver; drop their ids.
        this->impl_->erase_owned(this->impl_->command_buffers, [&](const impl::command_buffer_data& d) { return d.pool_id == pool; });
        this->impl_->command_pools.erase(it);
    }

    int32_t vulkan_host::allocate_command_buffer(uint64_t device, uint64_t pool, uint32_t level, uint64_t& out_command_buffer)
    {
        out_command_buffer = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->command_pools.find(pool);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->command_pools.end() || !dev->second.allocate_command_buffers)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandBufferAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = pool_it->second.handle;
        info.level = (level == 1) ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;

        VkCommandBuffer command_buffer{};
        const VkResult result = dev->second.allocate_command_buffers(dev->second.handle, &info, &command_buffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->command_buffers.emplace(id, impl::command_buffer_data{.handle = command_buffer, .device_id = device, .pool_id = pool});
        out_command_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_command_buffer(uint64_t device, uint64_t pool, uint64_t command_buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->command_pools.find(pool);
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && pool_it != this->impl_->command_pools.end() && cb->second.handle &&
            dev->second.free_command_buffers)
        {
            dev->second.free_command_buffers(dev->second.handle, pool_it->second.handle, 1, &cb->second.handle);
        }

        this->impl_->command_buffers.erase(cb);
    }

    int32_t vulkan_host::begin_command_buffer(uint64_t command_buffer, uint32_t flags, bool is_secondary, uint32_t view_mask,
                                              std::span<const uint32_t> color_formats, uint32_t depth_format, uint32_t stencil_format,
                                              uint32_t rasterization_samples, uint32_t rendering_flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.begin_command_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = flags;

        // A secondary command buffer recorded inside dynamic rendering needs an inheritance chain describing
        // the attachment formats it will render to (VkCommandBufferInheritanceRenderingInfo).
        VkCommandBufferInheritanceInfo inheritance{};
        VkCommandBufferInheritanceRenderingInfo rendering{};
        std::vector<VkFormat> color_format_handles;
        if (is_secondary)
        {
            color_format_handles.reserve(color_formats.size());
            for (const uint32_t f : color_formats)
            {
                color_format_handles.push_back(static_cast<VkFormat>(f));
            }

            rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            rendering.flags = rendering_flags;
            rendering.viewMask = view_mask;
            rendering.colorAttachmentCount = static_cast<uint32_t>(color_format_handles.size());
            rendering.pColorAttachmentFormats = color_format_handles.empty() ? nullptr : color_format_handles.data();
            rendering.depthAttachmentFormat = static_cast<VkFormat>(depth_format);
            rendering.stencilAttachmentFormat = static_cast<VkFormat>(stencil_format);
            rendering.rasterizationSamples =
                rasterization_samples ? static_cast<VkSampleCountFlagBits>(rasterization_samples) : VK_SAMPLE_COUNT_1_BIT;

            inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inheritance.pNext = &rendering;
            info.pInheritanceInfo = &inheritance;
        }

        return dev->second.begin_command_buffer(cb->second.handle, &info);
    }

    int32_t vulkan_host::cmd_execute_commands(uint64_t command_buffer, std::span<const uint64_t> secondaries)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end() || secondaries.empty())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_execute_commands)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkCommandBuffer> handles;
        handles.reserve(secondaries.size());
        for (const uint64_t id : secondaries)
        {
            const auto it = this->impl_->command_buffers.find(id);
            if (it == this->impl_->command_buffers.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles.push_back(it->second.handle);
        }

        dev->second.cmd_execute_commands(cb->second.handle, static_cast<uint32_t>(handles.size()), handles.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::end_command_buffer(uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.end_command_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.end_command_buffer(cb->second.handle);
    }

    int32_t vulkan_host::reset_command_pool(uint64_t device, uint64_t pool, uint32_t flags)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.reset_command_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto it = this->impl_->command_pools.find(pool);
        if (it == this->impl_->command_pools.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_command_pool(dev->second.handle, it->second.handle, flags);
    }

    int32_t vulkan_host::reset_command_buffer(uint64_t command_buffer, uint32_t flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.reset_command_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_command_buffer(cb->second.handle, flags);
    }

    int32_t vulkan_host::queue_wait_idle(uint64_t queue)
    {
        const auto queue_it = this->impl_->queues.find(queue);
        if (queue_it == this->impl_->queues.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(queue_it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.queue_wait_idle)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.queue_wait_idle(queue_it->second.handle);
    }

    int32_t vulkan_host::device_wait_idle(uint64_t device)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.device_wait_idle)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.device_wait_idle(dev->second.handle);
    }

    int32_t vulkan_host::create_fence(uint64_t device, uint32_t flags, uint64_t& out_fence)
    {
        out_fence = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_fence)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = flags;

        VkFence fence{};
        const VkResult result = dev->second.create_fence(dev->second.handle, &info, nullptr, &fence);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->fences.emplace(id, impl::fence_data{.handle = fence, .device_id = device});
        out_fence = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_fence(uint64_t device, uint64_t fence)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->fences.find(fence);
        if (it == this->impl_->fences.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_fence)
        {
            dev->second.destroy_fence(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->fences.erase(it);
    }

    int32_t vulkan_host::create_semaphore(uint64_t device, uint32_t flags, uint32_t semaphore_type, uint64_t initial_value,
                                          uint64_t& out_semaphore)
    {
        out_semaphore = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_semaphore)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = static_cast<VkSemaphoreType>(semaphore_type);
        type_info.initialValue = initial_value;

        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.flags = flags;
        if (semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE)
        {
            info.pNext = &type_info;
        }

        VkSemaphore semaphore{};
        const VkResult result = dev->second.create_semaphore(dev->second.handle, &info, nullptr, &semaphore);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->semaphores.emplace(id, impl::semaphore_data{.handle = semaphore, .device_id = device});
        out_semaphore = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_semaphore_counter_value(uint64_t device, uint64_t semaphore, uint64_t& out_value)
    {
        out_value = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->semaphores.find(semaphore);
        if (dev == this->impl_->devices.end() || it == this->impl_->semaphores.end() || !dev->second.get_semaphore_counter_value)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return dev->second.get_semaphore_counter_value(dev->second.handle, it->second.handle, &out_value);
    }

    int32_t vulkan_host::signal_semaphore(uint64_t device, uint64_t semaphore, uint64_t value)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->semaphores.find(semaphore);
        if (dev == this->impl_->devices.end() || it == this->impl_->semaphores.end() || !dev->second.signal_semaphore)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkSemaphoreSignalInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        info.semaphore = it->second.handle;
        info.value = value;
        return dev->second.signal_semaphore(dev->second.handle, &info);
    }

    int32_t vulkan_host::wait_semaphores(uint64_t device, uint32_t flags, const void* entries, uint32_t count, uint64_t timeout)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.wait_semaphores)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* records = static_cast<const gpu_bridge::wait_semaphore_entry*>(entries);
        std::vector<VkSemaphore> handles;
        std::vector<uint64_t> values;
        handles.reserve(count);
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto it = this->impl_->semaphores.find(records[i].semaphore);
            if (it == this->impl_->semaphores.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles.push_back(it->second.handle);
            values.push_back(records[i].value);
        }

        VkSemaphoreWaitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.flags = flags;
        info.semaphoreCount = count;
        info.pSemaphores = handles.data();
        info.pValues = values.data();
        return dev->second.wait_semaphores(dev->second.handle, &info, timeout);
    }

    uint64_t vulkan_host::get_buffer_device_address(uint64_t device, uint64_t buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffers.find(buffer);
        if (dev == this->impl_->devices.end() || it == this->impl_->buffers.end() || !dev->second.get_buffer_device_address)
        {
            return 0;
        }
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = it->second.handle;
        return dev->second.get_buffer_device_address(dev->second.handle, &info);
    }

    void vulkan_host::destroy_semaphore(uint64_t device, uint64_t semaphore)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->semaphores.find(semaphore);
        if (it == this->impl_->semaphores.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_semaphore)
        {
            dev->second.destroy_semaphore(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->semaphores.erase(it);
    }

    int32_t vulkan_host::reset_fence(uint64_t device, uint64_t fence)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->fences.find(fence);
        if (dev == this->impl_->devices.end() || it == this->impl_->fences.end() || !dev->second.reset_fences)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_fences(dev->second.handle, 1, &it->second.handle);
    }

    int32_t vulkan_host::get_fence_status(uint64_t fence)
    {
        const auto it = this->impl_->fences.find(fence);
        if (it == this->impl_->fences.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.get_fence_status)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.get_fence_status(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::create_event(uint64_t device, uint32_t flags, uint64_t& out_event)
    {
        out_event = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_event)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkEventCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        info.flags = flags;

        VkEvent event{};
        const VkResult result = dev->second.create_event(dev->second.handle, &info, nullptr, &event);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->events.emplace(id, impl::event_data{.handle = event, .device_id = device});
        out_event = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_event(uint64_t device, uint64_t event)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->events.find(event);
        if (it == this->impl_->events.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_event)
        {
            dev->second.destroy_event(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->events.erase(it);
    }

    int32_t vulkan_host::get_event_status(uint64_t event)
    {
        const auto it = this->impl_->events.find(event);
        if (it == this->impl_->events.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // GPU-side event ops (vkCmdSetEvent2/ResetEvent2/WaitEvents2) are not recorded through the bridge,
        // so the real VkEvent is never signalled by the GPU. DXVK only uses these events to poll for the
        // completion of work the bridge has already executed by the time the guest gets here, so report the
        // event as set. (If precise GPU event ordering is ever needed, record the cmd ops instead.)
        return VK_EVENT_SET;
    }

    int32_t vulkan_host::set_event(uint64_t device, uint64_t event)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->events.find(event);
        if (dev == this->impl_->devices.end() || it == this->impl_->events.end() || !dev->second.set_event)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.set_event(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::reset_event(uint64_t device, uint64_t event)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->events.find(event);
        if (dev == this->impl_->devices.end() || it == this->impl_->events.end() || !dev->second.reset_event)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_event(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::queue_submit(uint64_t queue, uint64_t command_buffer, uint64_t fence)
    {
        const auto queue_it = this->impl_->queues.find(queue);
        if (queue_it == this->impl_->queues.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(queue_it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.queue_submit)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFence fence_handle = VK_NULL_HANDLE;
        if (fence != 0)
        {
            const auto fence_it = this->impl_->fences.find(fence);
            if (fence_it == this->impl_->fences.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = fence_it->second.handle;
        }

        // Zero-command-buffer submission: still a valid fence signal operation (no work batched).
        if (command_buffer == 0)
        {
            return dev->second.queue_submit(queue_it->second.handle, 0, nullptr, fence_handle);
        }

        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb->second.handle;

        return dev->second.queue_submit(queue_it->second.handle, 1, &submit, fence_handle);
    }

    int32_t vulkan_host::queue_submit2(uint64_t queue, uint64_t fence, const void* wait_entries, uint32_t wait_count,
                                       const void* command_buffer_ids, uint32_t command_buffer_count, const void* signal_entries,
                                       uint32_t signal_count)
    {
        const auto queue_it = this->impl_->queues.find(queue);
        if (queue_it == this->impl_->queues.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(queue_it->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.queue_submit2)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkFence fence_handle = VK_NULL_HANDLE;
        if (fence != 0)
        {
            const auto fence_it = this->impl_->fences.find(fence);
            if (fence_it == this->impl_->fences.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = fence_it->second.handle;
        }

        const auto build_semaphores = [&](const void* entries, uint32_t count, std::vector<VkSemaphoreSubmitInfo>& out) {
            const auto* records = static_cast<const gpu_bridge::submit2_semaphore_entry*>(entries);
            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto sem = this->impl_->semaphores.find(records[i].semaphore);
                if (sem == this->impl_->semaphores.end())
                {
                    return false;
                }
                VkSemaphoreSubmitInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                info.semaphore = sem->second.handle;
                info.value = records[i].value;
                info.stageMask = records[i].stage_mask;
                out.push_back(info);
            }
            return true;
        };

        std::vector<VkSemaphoreSubmitInfo> waits;
        std::vector<VkSemaphoreSubmitInfo> signals;
        if (!build_semaphores(wait_entries, wait_count, waits) || !build_semaphores(signal_entries, signal_count, signals))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* cmd_ids = static_cast<const uint64_t*>(command_buffer_ids);
        std::vector<VkCommandBufferSubmitInfo> command_buffers;
        command_buffers.reserve(command_buffer_count);
        for (uint32_t i = 0; i < command_buffer_count; ++i)
        {
            const auto cb = this->impl_->command_buffers.find(cmd_ids[i]);
            if (cb == this->impl_->command_buffers.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            VkCommandBufferSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            info.commandBuffer = cb->second.handle;
            command_buffers.push_back(info);
        }

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = static_cast<uint32_t>(command_buffers.size());
        submit.pCommandBufferInfos = command_buffers.data();
        submit.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
        submit.pSignalSemaphoreInfos = signals.data();

        return dev->second.queue_submit2(queue_it->second.handle, 1, &submit, fence_handle);
    }

    int32_t vulkan_host::get_physical_device_memory_properties(uint64_t physical_device, void* out, size_t out_size)
    {
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_memory_properties)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceMemoryProperties properties{};
        instance->second.get_physical_device_memory_properties(pd->second.handle, &properties);

        std::memcpy(out, &properties, std::min(out_size, sizeof(properties)));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::allocate_memory(uint64_t device, uint64_t size, uint32_t memory_type_index, uint64_t& out_memory)
    {
        out_memory = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.allocate_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = size;
        info.memoryTypeIndex = memory_type_index;

        VkDeviceMemory memory{};
        const VkResult result = dev->second.allocate_memory(dev->second.handle, &info, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->memories.emplace(id, impl::memory_data{.handle = memory, .device_id = device, .allocation_size = size});
        out_memory = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_memory(uint64_t device, uint64_t memory)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->memories.find(memory);
        if (it == this->impl_->memories.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.free_memory)
        {
            dev->second.free_memory(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->memories.erase(it);
    }

    int32_t vulkan_host::create_buffer(uint64_t device, uint64_t size, uint32_t usage, uint64_t& out_buffer)
    {
        out_buffer = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer{};
        const VkResult result = dev->second.create_buffer(dev->second.handle, &info, nullptr, &buffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->buffers.emplace(id, impl::buffer_data{.handle = buffer, .device_id = device, .size = size});
        out_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_buffer(uint64_t device, uint64_t buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffers.find(buffer);
        if (it == this->impl_->buffers.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_buffer)
        {
            dev->second.destroy_buffer(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->buffers.erase(it);
    }

    int32_t vulkan_host::get_buffer_memory_requirements(uint64_t device, uint64_t buffer, uint64_t& out_size, uint64_t& out_alignment,
                                                        uint32_t& out_memory_type_bits)
    {
        out_size = 0;
        out_alignment = 0;
        out_memory_type_bits = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto buf = this->impl_->buffers.find(buffer);
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || !dev->second.get_buffer_memory_requirements)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryRequirements requirements{};
        dev->second.get_buffer_memory_requirements(dev->second.handle, buf->second.handle, &requirements);

        out_size = requirements.size;
        out_alignment = requirements.alignment;
        out_memory_type_bits = requirements.memoryTypeBits;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::bind_buffer_memory(uint64_t device, uint64_t buffer, uint64_t memory, uint64_t offset)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto buf = this->impl_->buffers.find(buffer);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || mem == this->impl_->memories.end() ||
            !dev->second.bind_buffer_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        buf->second.memory_id = memory;
        buf->second.memory_offset = offset;
        return dev->second.bind_buffer_memory(dev->second.handle, buf->second.handle, mem->second.handle, offset);
    }

    int32_t vulkan_host::cmd_fill_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t size, uint32_t data)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_fill_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_fill_buffer(cb->second.handle, buf->second.handle, offset, size, data);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::download_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, void* out, size_t out_size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.map_memory || !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), out_size);

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory(dev->second.handle, mem->second.handle, offset, size, 0, &mapped);
        if (result != VK_SUCCESS || !mapped)
        {
            return result != VK_SUCCESS ? result : VK_ERROR_MEMORY_MAP_FAILED;
        }

        std::memcpy(out, mapped, copy_bytes);
        dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::upload_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, const void* data, size_t data_size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.map_memory || !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), data_size);

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory(dev->second.handle, mem->second.handle, offset, size, 0, &mapped);
        if (result != VK_SUCCESS || !mapped)
        {
            return result != VK_SUCCESS ? result : VK_ERROR_MEMORY_MAP_FAILED;
        }

        std::memcpy(mapped, data, copy_bytes);
        dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::flush_mapped_memory_range(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.flush_mapped_memory_ranges)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = mem->second.handle;
        range.offset = offset;
        range.size = size;
        return dev->second.flush_mapped_memory_ranges(dev->second.handle, 1, &range);
    }

    int32_t vulkan_host::invalidate_mapped_memory_range(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.invalidate_mapped_memory_ranges)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = mem->second.handle;
        range.offset = offset;
        range.size = size;
        return dev->second.invalidate_mapped_memory_ranges(dev->second.handle, 1, &range);
    }

    int32_t vulkan_host::map_memory(uint64_t device, uint64_t memory, void*& out_host_pointer, uint64_t& out_size)
    {
        out_host_pointer = nullptr;
        out_size = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.map_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // A VkDeviceMemory must not be mapped twice; reuse the existing whole-object mapping.
        if (mem->second.persistent_host_pointer)
        {
            out_host_pointer = mem->second.persistent_host_pointer;
            out_size = mem->second.mapped_size;
            return VK_SUCCESS;
        }

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory(dev->second.handle, mem->second.handle, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS || !mapped)
        {
            return result != VK_SUCCESS ? result : VK_ERROR_MEMORY_MAP_FAILED;
        }

        out_host_pointer = mapped;
        out_size = mem->second.allocation_size;
        mem->second.persistent_host_pointer = mapped;
        mem->second.mapped_size = mem->second.allocation_size;
        return VK_SUCCESS;
    }

    void vulkan_host::unmap_memory(uint64_t device, uint64_t memory)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || !dev->second.unmap_memory)
        {
            return;
        }
        dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        mem->second.persistent_host_pointer = nullptr;
        mem->second.mapped_size = 0;
    }

    int32_t vulkan_host::create_image(uint64_t device, uint32_t format, uint32_t width, uint32_t height, uint32_t usage, uint32_t tiling,
                                      uint32_t samples, uint32_t image_type, uint32_t depth, uint32_t mip_levels, uint32_t array_layers,
                                      uint32_t flags, uint64_t& out_image)
    {
        out_image = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.flags = flags;
        info.imageType = static_cast<VkImageType>(image_type);
        info.format = static_cast<VkFormat>(format);
        info.extent = {.width = width, .height = height, .depth = depth ? depth : 1};
        info.mipLevels = mip_levels ? mip_levels : 1;
        info.arrayLayers = array_layers ? array_layers : 1;
        const auto sample_count = samples != 0 ? samples : static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT);
        info.samples = static_cast<VkSampleCountFlagBits>(sample_count);
        info.tiling = static_cast<VkImageTiling>(tiling);
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image{};
        const VkResult result = dev->second.create_image(dev->second.handle, &info, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->images.emplace(id, impl::image_data{.handle = image, .device_id = device, .samples = samples ? samples : 1});
        out_image = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image(uint64_t device, uint64_t image)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->images.find(image);
        if (it == this->impl_->images.end())
        {
            return;
        }

        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_image)
        {
            dev->second.destroy_image(dev->second.handle, it->second.handle, nullptr);
        }

        this->impl_->images.erase(it);
    }

    int32_t vulkan_host::get_image_memory_requirements(uint64_t device, uint64_t image, uint64_t& out_size, uint64_t& out_alignment,
                                                       uint32_t& out_memory_type_bits)
    {
        out_size = 0;
        out_alignment = 0;
        out_memory_type_bits = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || !dev->second.get_image_memory_requirements)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryRequirements requirements{};
        dev->second.get_image_memory_requirements(dev->second.handle, img->second.handle, &requirements);

        out_size = requirements.size;
        out_alignment = requirements.alignment;
        out_memory_type_bits = requirements.memoryTypeBits;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_image_subresource_layout(uint64_t device, uint64_t image, uint32_t aspect_mask, uint32_t mip_level,
                                                      uint32_t array_layer, uint64_t& out_offset, uint64_t& out_size,
                                                      uint64_t& out_row_pitch, uint64_t& out_array_pitch, uint64_t& out_depth_pitch)
    {
        out_offset = 0;
        out_size = 0;
        out_row_pitch = 0;
        out_array_pitch = 0;
        out_depth_pitch = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || !dev->second.get_image_subresource_layout)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageSubresource subresource{};
        subresource.aspectMask = aspect_mask != 0 ? aspect_mask : static_cast<uint32_t>(VK_IMAGE_ASPECT_COLOR_BIT);
        subresource.mipLevel = mip_level;
        subresource.arrayLayer = array_layer;

        VkSubresourceLayout layout{};
        dev->second.get_image_subresource_layout(dev->second.handle, img->second.handle, &subresource, &layout);

        out_offset = layout.offset;
        out_size = layout.size;
        out_row_pitch = layout.rowPitch;
        out_array_pitch = layout.arrayPitch;
        out_depth_pitch = layout.depthPitch;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::bind_image_memory(uint64_t device, uint64_t image, uint64_t memory, uint64_t offset)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || mem == this->impl_->memories.end() ||
            !dev->second.bind_image_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.bind_image_memory(dev->second.handle, img->second.handle, mem->second.handle, offset);
    }

    namespace
    {
        VkImageSubresourceRange to_vk_range(const vulkan_host::subresource_range& range)
        {
            VkImageSubresourceRange out{};
            out.aspectMask = range.aspect_mask;
            out.baseMipLevel = range.base_mip_level;
            out.levelCount = range.level_count;
            out.baseArrayLayer = range.base_array_layer;
            out.layerCount = range.layer_count;
            return out;
        }

        // The bridge's swapchain images are ordinary offscreen VkImages with no real presentation
        // engine, so the host driver would reject VK_IMAGE_LAYOUT_PRESENT_SRC_KHR. Map it to
        // TRANSFER_SRC_OPTIMAL (the layout the present-time readback copy reads from), keeping the guest
        // a faithful WSI app while the real driver stays valid.
        VkImageLayout translate_layout(uint32_t layout)
        {
            if (layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            {
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            }
            return static_cast<VkImageLayout>(layout);
        }
    }

    int32_t vulkan_host::cmd_pipeline_barrier(uint64_t command_buffer, uint64_t image, uint32_t src_stage_mask, uint32_t dst_stage_mask,
                                              uint32_t src_access_mask, uint32_t dst_access_mask, uint32_t old_layout, uint32_t new_layout,
                                              const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_pipeline_barrier)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = src_access_mask;
        barrier.dstAccessMask = dst_access_mask;
        barrier.oldLayout = translate_layout(old_layout);
        barrier.newLayout = translate_layout(new_layout);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img->second.handle;
        barrier.subresourceRange = to_vk_range(range);

        dev->second.cmd_pipeline_barrier(cb->second.handle, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_clear_color_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float r, float g, float b,
                                               float a, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_clear_color_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkClearColorValue clear{};
        clear.float32[0] = r;
        clear.float32[1] = g;
        clear.float32[2] = b;
        clear.float32[3] = a;

        const VkImageSubresourceRange vk_range = to_vk_range(range);
        dev->second.cmd_clear_color_image(cb->second.handle, img->second.handle, translate_layout(image_layout), &clear, 1, &vk_range);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_clear_depth_stencil_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float depth,
                                                       uint32_t stencil, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_clear_depth_stencil_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkClearDepthStencilValue clear{};
        clear.depth = depth;
        clear.stencil = stencil;

        const VkImageSubresourceRange vk_range = to_vk_range(range);
        dev->second.cmd_clear_depth_stencil_image(cb->second.handle, img->second.handle, translate_layout(image_layout), &clear, 1,
                                                  &vk_range);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_copy_image_to_buffer(uint64_t command_buffer, uint64_t image, uint32_t image_layout, uint64_t buffer,
                                                  uint32_t width, uint32_t height, uint32_t aspect_mask)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_image_to_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0; // tightly packed
        region.imageSubresource.aspectMask = aspect_mask;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {.x = 0, .y = 0, .z = 0};
        region.imageExtent = {.width = width, .height = height, .depth = 1};

        dev->second.cmd_copy_image_to_buffer(cb->second.handle, img->second.handle, translate_layout(image_layout), buf->second.handle, 1,
                                             &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_resolve_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image,
                                           uint32_t dst_layout, uint32_t width, uint32_t height, uint32_t aspect_mask)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto src = this->impl_->images.find(src_image);
        const auto dst = this->impl_->images.find(dst_image);
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->images.end() || dst == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_resolve_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageResolve region{};
        region.srcSubresource = {.aspectMask = aspect_mask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.srcOffset = {.x = 0, .y = 0, .z = 0};
        region.dstSubresource = {.aspectMask = aspect_mask, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.dstOffset = {.x = 0, .y = 0, .z = 0};
        region.extent = {.width = width, .height = height, .depth = 1};

        dev->second.cmd_resolve_image(cb->second.handle, src->second.handle, translate_layout(src_layout), dst->second.handle,
                                      translate_layout(dst_layout), 1, &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_update_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, const void* data, uint32_t size)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_update_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_update_buffer(cb->second.handle, buf->second.handle, offset, size, data);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_copy_buffer_to_image(uint64_t command_buffer, uint64_t buffer, uint64_t image, uint32_t image_layout,
                                                  const buffer_image_copy_region& r)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || img == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_buffer_to_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferImageCopy region{};
        region.bufferOffset = r.buffer_offset;
        region.bufferRowLength = r.buffer_row_length;
        region.bufferImageHeight = r.buffer_image_height;
        region.imageSubresource.aspectMask = r.aspect_mask;
        region.imageSubresource.mipLevel = r.mip_level;
        region.imageSubresource.baseArrayLayer = r.base_array_layer;
        region.imageSubresource.layerCount = r.layer_count ? r.layer_count : 1;
        region.imageOffset = {.x = r.image_offset_x, .y = r.image_offset_y, .z = r.image_offset_z};
        region.imageExtent = {.width = r.width, .height = r.height, .depth = r.depth ? r.depth : 1};

        dev->second.cmd_copy_buffer_to_image(cb->second.handle, buf->second.handle, img->second.handle, translate_layout(image_layout), 1,
                                             &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_copy_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image,
                                        uint32_t dst_layout, const image_copy_region& r)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto src = this->impl_->images.find(src_image);
        const auto dst = this->impl_->images.find(dst_image);
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->images.end() || dst == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageCopy region{};
        region.srcSubresource = {.aspectMask = r.src_aspect_mask,
                                 .mipLevel = r.src_mip_level,
                                 .baseArrayLayer = r.src_base_array_layer,
                                 .layerCount = r.src_layer_count ? r.src_layer_count : 1};
        region.srcOffset = {.x = r.src_offset_x, .y = r.src_offset_y, .z = r.src_offset_z};
        region.dstSubresource = {.aspectMask = r.dst_aspect_mask,
                                 .mipLevel = r.dst_mip_level,
                                 .baseArrayLayer = r.dst_base_array_layer,
                                 .layerCount = r.dst_layer_count ? r.dst_layer_count : 1};
        region.dstOffset = {.x = r.dst_offset_x, .y = r.dst_offset_y, .z = r.dst_offset_z};
        region.extent = {.width = r.width, .height = r.height, .depth = r.depth ? r.depth : 1};

        dev->second.cmd_copy_image(cb->second.handle, src->second.handle, translate_layout(src_layout), dst->second.handle,
                                   translate_layout(dst_layout), 1, &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_sampler(uint64_t device, uint32_t mag_filter, uint32_t min_filter, uint32_t address_mode_u,
                                        uint32_t address_mode_v, uint32_t address_mode_w, uint32_t mipmap_mode, uint32_t compare_enable,
                                        uint32_t compare_op, uint32_t anisotropy_enable, uint32_t border_color, float mip_lod_bias,
                                        float max_anisotropy, float min_lod, float max_lod, uint64_t& out_sampler)
    {
        out_sampler = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_sampler)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Custom border colors carry their RGBA in a VkSamplerCustomBorderColorCreateInfoEXT pNext that the
        // bridge does not forward; without it the enum is invalid, so fall back to opaque black.
        VkBorderColor border = static_cast<VkBorderColor>(border_color);
        if (border == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || border == VK_BORDER_COLOR_INT_CUSTOM_EXT)
        {
            border = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        }

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = static_cast<VkFilter>(mag_filter);
        info.minFilter = static_cast<VkFilter>(min_filter);
        info.mipmapMode = static_cast<VkSamplerMipmapMode>(mipmap_mode);
        info.addressModeU = static_cast<VkSamplerAddressMode>(address_mode_u);
        info.addressModeV = static_cast<VkSamplerAddressMode>(address_mode_v);
        info.addressModeW = static_cast<VkSamplerAddressMode>(address_mode_w);
        info.mipLodBias = mip_lod_bias;
        info.anisotropyEnable = anisotropy_enable ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = max_anisotropy < 1.0f ? 1.0f : max_anisotropy;
        // Depth-compare / PCF sampling (sampler2DShadow): a non-comparison sampler yields undefined results
        // here, which is what made shadow-mapped sun lighting render as fully shadowed.
        info.compareEnable = compare_enable ? VK_TRUE : VK_FALSE;
        info.compareOp = static_cast<VkCompareOp>(compare_op);
        info.minLod = min_lod;
        info.maxLod = max_lod;
        info.borderColor = border;

        VkSampler sampler{};
        const VkResult result = dev->second.create_sampler(dev->second.handle, &info, nullptr, &sampler);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->samplers.emplace(id, impl::sampler_data{.handle = sampler, .device_id = device});
        out_sampler = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_sampler(uint64_t device, uint64_t sampler)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->samplers.find(sampler);
        if (it == this->impl_->samplers.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_sampler)
        {
            dev->second.destroy_sampler(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->samplers.erase(it);
    }

    int32_t vulkan_host::create_surface(uint64_t hwnd, uint64_t& out_surface)
    {
        const uint64_t id = this->impl_->next_id++;
        this->impl_->surfaces.emplace(id, impl::surface_data{.hwnd = hwnd});
        out_surface = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_surface(uint64_t surface)
    {
        this->impl_->surfaces.erase(surface);
    }

    uint64_t vulkan_host::get_surface_hwnd(uint64_t surface) const
    {
        const auto it = this->impl_->surfaces.find(surface);
        return it == this->impl_->surfaces.end() ? 0 : it->second.hwnd;
    }

    int32_t vulkan_host::get_surface_capabilities(uint64_t /*physical_device*/, uint64_t surface, uint32_t window_width,
                                                  uint32_t window_height, void* out, size_t out_size)
    {
        if (!this->impl_->surfaces.contains(surface))
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        VkSurfaceCapabilitiesKHR caps{};
        caps.minImageCount = 2;
        caps.maxImageCount = 0; // no upper bound
        // Report the guest window's client size as the current extent. Returning the "undefined" value
        // (0xFFFFFFFF) made DXVK fall back to querying the window size itself and end up with a 0x0
        // swapchain, so nothing was ever presented (black window). A concrete extent fixes that.
        caps.currentExtent = (window_width != 0 && window_height != 0) ? VkExtent2D{.width = window_width, .height = window_height}
                                                                       : VkExtent2D{.width = 0xFFFFFFFFu, .height = 0xFFFFFFFFu};
        caps.minImageExtent = {.width = 1, .height = 1};
        caps.maxImageExtent = {.width = 16384, .height = 16384};
        caps.maxImageArrayLayers = 1;
        caps.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        caps.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        std::memcpy(out, &caps, std::min(out_size, sizeof(caps)));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_swapchain(uint64_t device, uint64_t surface, uint32_t format, uint32_t width, uint32_t height,
                                          uint32_t min_image_count, uint32_t image_usage, uint64_t& out_swapchain,
                                          uint32_t& out_image_count)
    {
        out_swapchain = 0;
        out_image_count = 0;

        const auto dev_it = this->impl_->devices.find(device);
        const auto surf_it = this->impl_->surfaces.find(surface);
        if (dev_it == this->impl_->devices.end() || surf_it == this->impl_->surfaces.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;
        if (!dev.create_image || !dev.allocate_memory || !dev.bind_image_memory || !dev.create_buffer || !dev.bind_buffer_memory ||
            !dev.create_command_pool || !dev.allocate_command_buffers || !dev.create_fence)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const uint32_t image_count = (min_image_count < 2) ? 2 : min_image_count;
        const auto vk_format = static_cast<VkFormat>(format);

        impl::swapchain_data sc{};
        sc.device_id = device;
        sc.hwnd = surf_it->second.hwnd;
        sc.width = width;
        sc.height = height;
        sc.format = vk_format;

        const auto fail = [&]() -> int32_t {
            this->impl_->destroy_swapchain_resources(sc);
            return VK_ERROR_INITIALIZATION_FAILED;
        };

        // Offscreen images that stand in for the swapchain's presentable images. TRANSFER_SRC is always
        // added so the present-time readback copy can source them.
        for (uint32_t i = 0; i < image_count; ++i)
        {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = vk_format;
            info.extent = {.width = width, .height = height, .depth = 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = image_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage image{};
            if (dev.create_image(dev.handle, &info, nullptr, &image) != VK_SUCCESS)
            {
                return fail();
            }

            VkMemoryRequirements reqs{};
            dev.get_image_memory_requirements(dev.handle, image, &reqs);
            uint32_t type_index = this->impl_->find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type_index == UINT32_MAX)
            {
                type_index = this->impl_->find_memory_type(dev, reqs.memoryTypeBits, 0);
            }

            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = reqs.size;
            alloc.memoryTypeIndex = type_index;
            VkDeviceMemory memory{};
            if (dev.allocate_memory(dev.handle, &alloc, nullptr, &memory) != VK_SUCCESS)
            {
                dev.destroy_image(dev.handle, image, nullptr);
                return fail();
            }
            dev.bind_image_memory(dev.handle, image, memory, 0);

            const uint64_t image_id = this->impl_->next_id++;
            this->impl_->images.emplace(image_id, impl::image_data{.handle = image, .device_id = device});
            sc.image_ids.push_back(image_id);
            sc.image_memory.push_back(memory);
        }

        // Host-visible readback buffer the presented image is copied into (4 bytes/pixel).
        const VkDeviceSize readback_size = static_cast<VkDeviceSize>(width) * height * 4;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = readback_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (dev.create_buffer(dev.handle, &buffer_info, nullptr, &sc.readback_buffer) != VK_SUCCESS)
        {
            return fail();
        }

        VkMemoryRequirements buffer_reqs{};
        dev.get_buffer_memory_requirements(dev.handle, sc.readback_buffer, &buffer_reqs);
        // Prefer HOST_CACHED for the readback buffer: presenting copies the frame here and the CPU then
        // reads it all back, and reads from uncached/write-combined (plain COHERENT) memory are an order
        // of magnitude slower. Keep COHERENT too so no manual vkInvalidateMappedMemoryRanges is needed;
        // fall back to plain HOST_VISIBLE|HOST_COHERENT if the driver offers no cached+coherent type.
        uint32_t buffer_type = this->impl_->find_memory_type(dev, buffer_reqs.memoryTypeBits,
                                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (buffer_type == UINT32_MAX)
        {
            buffer_type = this->impl_->find_memory_type(dev, buffer_reqs.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        if (buffer_type == UINT32_MAX)
        {
            return fail();
        }
        VkMemoryAllocateInfo buffer_alloc{};
        buffer_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_alloc.allocationSize = buffer_reqs.size;
        buffer_alloc.memoryTypeIndex = buffer_type;
        if (dev.allocate_memory(dev.handle, &buffer_alloc, nullptr, &sc.readback_memory) != VK_SUCCESS)
        {
            return fail();
        }
        dev.bind_buffer_memory(dev.handle, sc.readback_buffer, sc.readback_memory, 0);

        // Reusable command buffer + fence for the per-present readback copy.
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = dev.queue_family_index;
        if (dev.create_command_pool(dev.handle, &pool_info, nullptr, &sc.present_pool) != VK_SUCCESS)
        {
            return fail();
        }

        VkCommandBufferAllocateInfo cb_info{};
        cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb_info.commandPool = sc.present_pool;
        cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb_info.commandBufferCount = 1;
        if (dev.allocate_command_buffers(dev.handle, &cb_info, &sc.present_cmd) != VK_SUCCESS)
        {
            return fail();
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (dev.create_fence(dev.handle, &fence_info, nullptr, &sc.present_fence) != VK_SUCCESS)
        {
            return fail();
        }

        const uint64_t id = this->impl_->next_id++;
        out_image_count = image_count;
        this->impl_->swapchains.emplace(id, std::move(sc));
        out_swapchain = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_swapchain(uint64_t /*device*/, uint64_t swapchain)
    {
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end())
        {
            return;
        }
        this->impl_->destroy_swapchain_resources(it->second);
        this->impl_->swapchains.erase(it);
    }

    int32_t vulkan_host::get_swapchain_images(uint64_t swapchain, std::span<uint64_t> out_images, uint32_t& out_count)
    {
        out_count = 0;
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        out_count = static_cast<uint32_t>(it->second.image_ids.size());
        for (size_t i = 0; i < it->second.image_ids.size() && i < out_images.size(); ++i)
        {
            out_images[i] = it->second.image_ids[i];
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::acquire_next_image(uint64_t swapchain, uint64_t semaphore, uint64_t fence, uint32_t& out_index)
    {
        out_index = 0;
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end() || it->second.image_ids.empty())
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        // No real presentation engine: images are always available, handed out round-robin.
        out_index = it->second.next_image;
        it->second.next_image = (it->second.next_image + 1) % static_cast<uint32_t>(it->second.image_ids.size());

        if (semaphore == 0 && fence == 0)
        {
            return VK_SUCCESS;
        }

        // The caller's render submit waits on the acquire semaphore (and it may wait on the fence) before it
        // can run; with no present engine to signal them, an empty submit does so now that the image is ready.
        // Otherwise that submit -- and the frame fence it signals -- would never complete, deadlocking DXVK.
        const auto dev_it = this->impl_->devices.find(it->second.device_id);
        if (dev_it == this->impl_->devices.end() || !dev_it->second.queue_submit)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;

        VkSemaphore sem_handle = VK_NULL_HANDLE;
        if (semaphore != 0)
        {
            const auto sem_it = this->impl_->semaphores.find(semaphore);
            if (sem_it == this->impl_->semaphores.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            sem_handle = sem_it->second.handle;
        }

        VkFence fence_handle = VK_NULL_HANDLE;
        if (fence != 0)
        {
            const auto fence_it = this->impl_->fences.find(fence);
            if (fence_it == this->impl_->fences.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = fence_it->second.handle;
        }

        VkQueue queue = VK_NULL_HANDLE;
        for (const auto& [id, q] : this->impl_->queues)
        {
            if (q.device_id == it->second.device_id)
            {
                queue = q.handle;
                break;
            }
        }
        if (queue == VK_NULL_HANDLE)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (sem_handle != VK_NULL_HANDLE)
        {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &sem_handle;
        }

        if (dev.queue_submit(queue, 1, &submit, fence_handle) != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::queue_present(uint64_t queue, uint64_t swapchain, uint32_t image_index, std::vector<std::byte>& out_pixels,
                                       uint32_t& out_width, uint32_t& out_height, uint64_t& out_hwnd)
    {
        out_pixels.clear();
        out_width = 0;
        out_height = 0;
        out_hwnd = 0;

        const auto queue_it = this->impl_->queues.find(queue);
        const auto sc_it = this->impl_->swapchains.find(swapchain);
        if (queue_it == this->impl_->queues.end() || sc_it == this->impl_->swapchains.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::swapchain_data& sc = sc_it->second;
        if (image_index >= sc.image_ids.size())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        uint64_t source_image_id = sc.image_ids[image_index];

        const auto dev_it = this->impl_->devices.find(sc.device_id);
        const auto img_it = this->impl_->images.find(source_image_id);
        if (dev_it == this->impl_->devices.end() || img_it == this->impl_->images.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;
        if (!dev.begin_command_buffer || !dev.end_command_buffer || !dev.cmd_copy_image_to_buffer || !dev.queue_submit ||
            !dev.queue_wait_idle || !dev.get_fence_status || !dev.map_memory || !dev.unmap_memory || !dev.cmd_pipeline_barrier ||
            !dev.reset_fences)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkDeviceSize readback_size = static_cast<VkDeviceSize>(sc.width) * sc.height * 4;

        // Async present, part 1: drain the copy submitted on the *previous* present and hand its pixels
        // back to be shown now. A full guest frame has elapsed since it was submitted, so its fence is
        // essentially always already signalled (the GPU did the copy while the guest rendered the next
        // frame) -- only fall back to a blocking wait in the unlikely case it is not. The previous
        // synchronous vkQueueWaitIdle here was ~25% of the emulated frame; deferring keeps it off the
        // host thread's critical path. Present is therefore one frame behind, which is imperceptible.
        if (sc.present_in_flight)
        {
            if (dev.get_fence_status(dev.handle, sc.present_fence) != VK_SUCCESS)
            {
                dev.queue_wait_idle(queue_it->second.handle);
            }

            void* mapped = nullptr;
            if (dev.map_memory(dev.handle, sc.readback_memory, 0, readback_size, 0, &mapped) == VK_SUCCESS && mapped)
            {
                out_pixels.resize(static_cast<size_t>(readback_size));
                std::memcpy(out_pixels.data(), mapped, out_pixels.size());
                dev.unmap_memory(dev.handle, sc.readback_memory);
                out_width = sc.width;
                out_height = sc.height;
                out_hwnd = sc.hwnd;
            }
            sc.present_in_flight = false;
        }

        // Async present, part 2: record + submit the copy for the *current* frame, but do not wait on it.
        // It runs on the GPU while the guest proceeds; the next present drains it (above). The previous
        // copy is guaranteed finished here (the drain checked its fence), so reusing present_cmd is safe.
        // Make the rendered image's writes visible to the copy, then copy it into the readback buffer.
        // The guest left the image in PRESENT_SRC, which the bridge mapped to TRANSFER_SRC_OPTIMAL.
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dev.begin_command_buffer(sc.present_cmd, &begin) != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = img_it->second.handle;
        barrier.subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        dev.cmd_pipeline_barrier(sc.present_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &barrier);

        VkImage copy_image = img_it->second.handle;
        VkImageLayout copy_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const uint32_t copy_w = sc.width;
        const uint32_t copy_h = sc.height;

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {.x = 0, .y = 0, .z = 0};
        region.imageExtent = {.width = copy_w, .height = copy_h, .depth = 1};
        dev.cmd_copy_image_to_buffer(sc.present_cmd, copy_image, copy_layout, sc.readback_buffer, 1, &region);
        dev.end_command_buffer(sc.present_cmd);

        dev.reset_fences(dev.handle, 1, &sc.present_fence);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &sc.present_cmd;
        if (dev.queue_submit(queue_it->second.handle, 1, &submit, sc.present_fence) != VK_SUCCESS)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        sc.present_in_flight = true;

        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_shader_module(uint64_t device, const void* code, size_t code_size, uint64_t& out_module)
    {
        out_module = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_shader_module)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code_size;
        info.pCode = static_cast<const uint32_t*>(code);

        VkShaderModule module{};
        const VkResult result = dev->second.create_shader_module(dev->second.handle, &info, nullptr, &module);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->shader_modules.emplace(id, impl::shader_module_data{.handle = module, .device_id = device});
        out_module = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_shader_module(uint64_t device, uint64_t shader_module)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->shader_modules.find(shader_module);
        if (it == this->impl_->shader_modules.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_shader_module)
        {
            dev->second.destroy_shader_module(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->shader_modules.erase(it);
    }

    int32_t vulkan_host::create_image_view(uint64_t device, uint64_t image, uint32_t format, uint32_t aspect_mask, uint32_t view_type,
                                           uint32_t base_mip_level, uint32_t level_count, uint32_t base_array_layer, uint32_t layer_count,
                                           uint32_t swizzle_r, uint32_t swizzle_g, uint32_t swizzle_b, uint32_t swizzle_a,
                                           uint64_t& out_view)
    {
        out_view = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || !dev->second.create_image_view)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = img->second.handle;
        info.viewType = static_cast<VkImageViewType>(view_type);
        info.format = static_cast<VkFormat>(format);
        info.components = {.r = static_cast<VkComponentSwizzle>(swizzle_r),
                           .g = static_cast<VkComponentSwizzle>(swizzle_g),
                           .b = static_cast<VkComponentSwizzle>(swizzle_b),
                           .a = static_cast<VkComponentSwizzle>(swizzle_a)};
        info.subresourceRange.aspectMask = (aspect_mask != 0) ? aspect_mask : static_cast<uint32_t>(VK_IMAGE_ASPECT_COLOR_BIT);
        info.subresourceRange.baseMipLevel = base_mip_level;
        info.subresourceRange.levelCount = level_count ? level_count : 1;
        info.subresourceRange.baseArrayLayer = base_array_layer;
        info.subresourceRange.layerCount = layer_count ? layer_count : 1;

        VkImageView view{};
        const VkResult result = dev->second.create_image_view(dev->second.handle, &info, nullptr, &view);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->image_views.emplace(id, impl::image_view_data{.handle = view, .device_id = device});
        out_view = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image_view(uint64_t device, uint64_t image_view)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->image_views.find(image_view);
        if (it == this->impl_->image_views.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_image_view)
        {
            dev->second.destroy_image_view(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->image_views.erase(it);
    }

    int32_t vulkan_host::create_buffer_view(uint64_t device, uint64_t buffer, uint32_t format, uint64_t offset, uint64_t range,
                                            uint64_t& out_view)
    {
        out_view = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto buf = this->impl_->buffers.find(buffer);
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || !dev->second.create_buffer_view)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkBufferViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        info.buffer = buf->second.handle;
        info.format = static_cast<VkFormat>(format);
        info.offset = offset;
        info.range = range ? range : VK_WHOLE_SIZE;

        VkBufferView view{};
        const VkResult result = dev->second.create_buffer_view(dev->second.handle, &info, nullptr, &view);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->buffer_views.emplace(id, impl::buffer_view_data{.handle = view, .device_id = device});
        out_view = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_buffer_view(uint64_t device, uint64_t buffer_view)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffer_views.find(buffer_view);
        if (it == this->impl_->buffer_views.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_buffer_view)
        {
            dev->second.destroy_buffer_view(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->buffer_views.erase(it);
    }

    int32_t vulkan_host::cmd_copy_buffer(uint64_t command_buffer, uint64_t src_buffer, uint64_t dst_buffer,
                                         std::span<const buffer_copy> regions)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto src = this->impl_->buffers.find(src_buffer);
        const auto dst = this->impl_->buffers.find(dst_buffer);
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->buffers.end() || dst == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_buffer || regions.empty())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkBufferCopy> vk_regions;
        vk_regions.reserve(regions.size());
        for (const auto& r : regions)
        {
            vk_regions.push_back(VkBufferCopy{.srcOffset = r.src_offset, .dstOffset = r.dst_offset, .size = r.size});
        }

        dev->second.cmd_copy_buffer(cb->second.handle, src->second.handle, dst->second.handle, static_cast<uint32_t>(vk_regions.size()),
                                    vk_regions.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_query_pool(uint64_t device, uint32_t query_type, uint32_t query_count, uint32_t pipeline_statistics,
                                           uint64_t& out_pool)
    {
        out_pool = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_query_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = static_cast<VkQueryType>(query_type);
        info.queryCount = query_count;
        info.pipelineStatistics = pipeline_statistics;

        VkQueryPool pool{};
        const VkResult result = dev->second.create_query_pool(dev->second.handle, &info, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->query_pools.emplace(id, impl::query_pool_data{.handle = pool, .device_id = device});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_query_pool(uint64_t device, uint64_t query_pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->query_pools.find(query_pool);
        if (it == this->impl_->query_pools.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_query_pool)
        {
            dev->second.destroy_query_pool(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->query_pools.erase(it);
    }

    int32_t vulkan_host::reset_query_pool(uint64_t device, uint64_t query_pool, uint32_t first_query, uint32_t query_count)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (dev == this->impl_->devices.end() || qp == this->impl_->query_pools.end() || !dev->second.reset_query_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.reset_query_pool(dev->second.handle, qp->second.handle, first_query, query_count);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_query_pool_results(uint64_t device, uint64_t query_pool, uint32_t first_query, uint32_t query_count,
                                                uint32_t flags, void* out, size_t out_size, size_t stride, size_t& out_written)
    {
        out_written = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (dev == this->impl_->devices.end() || qp == this->impl_->query_pools.end() || !dev->second.get_query_pool_results)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult result = dev->second.get_query_pool_results(dev->second.handle, qp->second.handle, first_query, query_count,
                                                                   out_size, out, stride, static_cast<VkQueryResultFlags>(flags));
        if (result == VK_SUCCESS)
        {
            out_written = out_size;
        }
        return result;
    }

    int32_t vulkan_host::cmd_reset_query_pool(uint64_t command_buffer, uint64_t query_pool, uint32_t first_query, uint32_t query_count)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_reset_query_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_reset_query_pool(cb->second.handle, qp->second.handle, first_query, query_count);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_begin_query(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_begin_query)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_begin_query(cb->second.handle, qp->second.handle, query, static_cast<VkQueryControlFlags>(flags));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_end_query(uint64_t command_buffer, uint64_t query_pool, uint32_t query)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_end_query)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_end_query(cb->second.handle, qp->second.handle, query);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_write_timestamp(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t pipeline_stage)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_write_timestamp)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_write_timestamp(cb->second.handle, static_cast<VkPipelineStageFlagBits>(pipeline_stage), qp->second.handle, query);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_render_pass(uint64_t device, uint32_t format, uint32_t load_op, uint32_t store_op, uint32_t initial_layout,
                                            uint32_t final_layout, uint32_t depth_format, uint64_t& out_render_pass)
    {
        out_render_pass = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const bool has_depth = depth_format != 0;

        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = static_cast<VkFormat>(format);
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = static_cast<VkAttachmentLoadOp>(load_op);
        attachments[0].storeOp = static_cast<VkAttachmentStoreOp>(store_op);
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = translate_layout(initial_layout);
        attachments[0].finalLayout = translate_layout(final_layout);
        // Depth attachment is cleared on load and not stored (transient).
        attachments[1].format = static_cast<VkFormat>(depth_format);
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{};
        color_ref.attachment = 0;
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depth_ref{};
        depth_ref.attachment = 1;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (has_depth)
        {
            // Also synchronize the depth attachment's load/store across the early/late fragment tests.
            dep.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = has_depth ? 2 : 1;
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dep;

        VkRenderPass render_pass{};
        const VkResult result = dev->second.create_render_pass(dev->second.handle, &info, nullptr, &render_pass);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->render_passes.emplace(id, impl::render_pass_data{.handle = render_pass, .device_id = device, .has_depth = has_depth});
        out_render_pass = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_render_pass(uint64_t device, uint64_t render_pass)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->render_passes.find(render_pass);
        if (it == this->impl_->render_passes.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_render_pass)
        {
            dev->second.destroy_render_pass(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->render_passes.erase(it);
    }

    int32_t vulkan_host::create_framebuffer(uint64_t device, uint64_t render_pass, uint64_t image_view, uint64_t depth_view, uint32_t width,
                                            uint32_t height, uint64_t& out_framebuffer)
    {
        out_framebuffer = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto rp = this->impl_->render_passes.find(render_pass);
        const auto view = this->impl_->image_views.find(image_view);
        if (dev == this->impl_->devices.end() || rp == this->impl_->render_passes.end() || view == this->impl_->image_views.end() ||
            !dev->second.create_framebuffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::array<VkImageView, 2> views{view->second.handle, VK_NULL_HANDLE};
        uint32_t attachment_count = 1;
        if (depth_view != 0)
        {
            const auto dview = this->impl_->image_views.find(depth_view);
            if (dview == this->impl_->image_views.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            views[1] = dview->second.handle;
            attachment_count = 2;
        }

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = rp->second.handle;
        info.attachmentCount = attachment_count;
        info.pAttachments = views.data();
        info.width = width;
        info.height = height;
        info.layers = 1;

        VkFramebuffer framebuffer{};
        const VkResult result = dev->second.create_framebuffer(dev->second.handle, &info, nullptr, &framebuffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->framebuffers.emplace(id, impl::framebuffer_data{.handle = framebuffer, .device_id = device});
        out_framebuffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_framebuffer(uint64_t device, uint64_t framebuffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->framebuffers.find(framebuffer);
        if (it == this->impl_->framebuffers.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_framebuffer)
        {
            dev->second.destroy_framebuffer(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->framebuffers.erase(it);
    }

    int32_t vulkan_host::create_pipeline_layout(uint64_t device, uint32_t push_constant_stages, uint32_t push_constant_size,
                                                std::span<const uint64_t> set_layouts, uint64_t& out_layout)
    {
        out_layout = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_pipeline_layout)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPushConstantRange push_range{};
        push_range.stageFlags = push_constant_stages;
        push_range.offset = 0;
        push_range.size = push_constant_size;

        std::vector<VkDescriptorSetLayout> vk_set_layouts;
        vk_set_layouts.reserve(set_layouts.size());
        for (const uint64_t id : set_layouts)
        {
            const auto sl = this->impl_->descriptor_set_layouts.find(id);
            if (sl == this->impl_->descriptor_set_layouts.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            vk_set_layouts.push_back(sl->second.handle);
        }

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = static_cast<uint32_t>(vk_set_layouts.size());
        info.pSetLayouts = vk_set_layouts.empty() ? nullptr : vk_set_layouts.data();
        if (push_constant_size > 0)
        {
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges = &push_range;
        }

        VkPipelineLayout layout{};
        const VkResult result = dev->second.create_pipeline_layout(dev->second.handle, &info, nullptr, &layout);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->pipeline_layouts.emplace(id, impl::pipeline_layout_data{.handle = layout, .device_id = device});
        out_layout = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline_layout(uint64_t device, uint64_t pipeline_layout)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (it == this->impl_->pipeline_layouts.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_pipeline_layout)
        {
            dev->second.destroy_pipeline_layout(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->pipeline_layouts.erase(it);
    }

    int32_t vulkan_host::create_descriptor_set_layout(uint64_t device, std::span<const descriptor_binding> bindings, uint64_t& out_layout)
    {
        out_layout = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_descriptor_set_layout)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
        vk_bindings.reserve(bindings.size());
        for (const descriptor_binding& b : bindings)
        {
            VkDescriptorSetLayoutBinding vb{};
            vb.binding = b.binding;
            vb.descriptorType = static_cast<VkDescriptorType>(b.descriptor_type);
            vb.descriptorCount = b.descriptor_count;
            vb.stageFlags = b.stage_flags;
            vk_bindings.push_back(vb);
        }

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(vk_bindings.size());
        info.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();

        VkDescriptorSetLayout layout{};
        const VkResult result = dev->second.create_descriptor_set_layout(dev->second.handle, &info, nullptr, &layout);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->descriptor_set_layouts.emplace(id, impl::descriptor_set_layout_data{.handle = layout, .device_id = device});
        out_layout = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_descriptor_set_layout(uint64_t device, uint64_t layout)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->descriptor_set_layouts.find(layout);
        if (it == this->impl_->descriptor_set_layouts.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_descriptor_set_layout)
        {
            dev->second.destroy_descriptor_set_layout(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->descriptor_set_layouts.erase(it);
    }

    int32_t vulkan_host::create_descriptor_pool(uint64_t device, uint32_t max_sets, std::span<const descriptor_pool_size> sizes,
                                                uint64_t& out_pool)
    {
        out_pool = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_descriptor_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorPoolSize> vk_sizes;
        vk_sizes.reserve(sizes.size());
        for (const descriptor_pool_size& s : sizes)
        {
            vk_sizes.push_back({.type = static_cast<VkDescriptorType>(s.descriptor_type), .descriptorCount = s.descriptor_count});
        }

        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = max_sets;
        info.poolSizeCount = static_cast<uint32_t>(vk_sizes.size());
        info.pPoolSizes = vk_sizes.empty() ? nullptr : vk_sizes.data();

        VkDescriptorPool pool{};
        const VkResult result = dev->second.create_descriptor_pool(dev->second.handle, &info, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->descriptor_pools.emplace(id, impl::descriptor_pool_data{.handle = pool, .device_id = device});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_descriptor_pool(uint64_t device, uint64_t pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->descriptor_pools.find(pool);
        if (it == this->impl_->descriptor_pools.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_descriptor_pool)
        {
            dev->second.destroy_descriptor_pool(dev->second.handle, it->second.handle, nullptr);
        }
        // Sets allocated from this pool are freed implicitly; drop their ids.
        this->impl_->erase_owned(this->impl_->descriptor_sets, [&](const impl::descriptor_set_data& d) { return d.pool_id == pool; });
        this->impl_->descriptor_pools.erase(it);
    }

    int32_t vulkan_host::allocate_descriptor_sets(uint64_t device, uint64_t pool, std::span<const uint64_t> set_layouts,
                                                  std::span<uint64_t> out_sets, uint32_t& out_count)
    {
        out_count = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->descriptor_pools.find(pool);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->descriptor_pools.end() || !dev->second.allocate_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSetLayout> vk_layouts;
        vk_layouts.reserve(set_layouts.size());
        for (const uint64_t id : set_layouts)
        {
            const auto sl = this->impl_->descriptor_set_layouts.find(id);
            if (sl == this->impl_->descriptor_set_layouts.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            vk_layouts.push_back(sl->second.handle);
        }

        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = pool_it->second.handle;
        info.descriptorSetCount = static_cast<uint32_t>(vk_layouts.size());
        info.pSetLayouts = vk_layouts.empty() ? nullptr : vk_layouts.data();

        std::vector<VkDescriptorSet> sets(vk_layouts.size());
        const VkResult result = dev->second.allocate_descriptor_sets(dev->second.handle, &info, sets.data());
        if (result != VK_SUCCESS)
        {
            return result;
        }

        out_count = static_cast<uint32_t>(sets.size());
        for (size_t i = 0; i < sets.size(); ++i)
        {
            const uint64_t id = this->impl_->next_id++;
            this->impl_->descriptor_sets.emplace(
                id, impl::descriptor_set_data{.handle = sets[i], .device_id = device, .pool_id = pool, .buffer_bindings = {}});
            if (i < out_sets.size())
            {
                out_sets[i] = id;
            }
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::update_descriptor_sets(uint64_t device, std::span<const descriptor_write> writes)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.update_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Each write carries exactly one descriptor. Buffer infos and image infos are kept in stable
        // vectors so the VkWriteDescriptorSet pointers remain valid until the driver call below.
        std::vector<VkWriteDescriptorSet> vk_writes;
        std::vector<VkDescriptorBufferInfo> buffer_infos(writes.size());
        std::vector<VkDescriptorImageInfo> image_infos(writes.size());
        vk_writes.reserve(writes.size());

        for (size_t i = 0; i < writes.size(); ++i)
        {
            const descriptor_write& w = writes[i];
            const auto set = this->impl_->descriptor_sets.find(w.dst_set);
            if (set == this->impl_->descriptor_sets.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkWriteDescriptorSet vw{};
            vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            vw.dstSet = set->second.handle;
            vw.dstBinding = w.dst_binding;
            vw.dstArrayElement = w.dst_array_element;
            vw.descriptorCount = 1;
            vw.descriptorType = static_cast<VkDescriptorType>(w.descriptor_type);

            const bool is_image =
                (w.descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                 w.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER);
            if (is_image)
            {
                const auto view = this->impl_->image_views.find(w.image_view);
                VkDescriptorImageInfo& ii = image_infos[i];
                ii.sampler = VK_NULL_HANDLE;
                if (w.sampler != 0)
                {
                    const auto smp = this->impl_->samplers.find(w.sampler);
                    if (smp == this->impl_->samplers.end())
                    {
                        return VK_ERROR_INITIALIZATION_FAILED;
                    }
                    ii.sampler = smp->second.handle;
                }
                ii.imageView = (view != this->impl_->image_views.end()) ? view->second.handle : VK_NULL_HANDLE;
                ii.imageLayout = static_cast<VkImageLayout>(w.image_layout);
                vw.pImageInfo = &ii;
            }
            else
            {
                VkDescriptorBufferInfo& bi = buffer_infos[i];
                if (w.buffer == 0)
                {
                    // Null descriptor (VK_EXT_robustness2 nullDescriptor, which DXVK enables): an unused
                    // uniform/storage buffer slot is bound to VK_NULL_HANDLE. Returning an error here makes
                    // the recorded command buffer fail, which DXVK reports as a fatal vkEndCommandBuffer error.
                    bi.buffer = VK_NULL_HANDLE;
                    bi.offset = 0;
                    bi.range = w.range ? w.range : VK_WHOLE_SIZE;
                }
                else
                {
                    const auto buf = this->impl_->buffers.find(w.buffer);
                    if (buf == this->impl_->buffers.end())
                    {
                        return VK_ERROR_INITIALIZATION_FAILED;
                    }
                    bi.buffer = buf->second.handle;
                    bi.offset = w.offset;
                    bi.range = w.range;
                    set->second.buffer_bindings[w.dst_binding] =
                        impl::bound_buffer_info{.buffer_id = w.buffer, .offset = w.offset, .range = w.range, .type = w.descriptor_type};
                }
                vw.pBufferInfo = &bi;
            }
            vk_writes.push_back(vw);
        }

        if (!vk_writes.empty())
        {
            dev->second.update_descriptor_sets(dev->second.handle, static_cast<uint32_t>(vk_writes.size()), vk_writes.data(), 0, nullptr);
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_graphics_pipeline(uint64_t device, uint64_t render_pass, uint64_t pipeline_layout, uint64_t vertex_shader,
                                                  uint64_t fragment_shader, uint32_t width, uint32_t height,
                                                  std::span<const vertex_binding> bindings, std::span<const vertex_attribute> attributes,
                                                  const depth_state& depth, std::span<const uint32_t> color_formats, uint32_t depth_format,
                                                  uint32_t stencil_format, uint32_t rasterization_samples,
                                                  std::span<const uint32_t> dynamic_states, const specialization& vs_spec,
                                                  const specialization& fs_spec,
                                                  std::span<const color_blend_attachment> blend_attachments_in, uint64_t& out_pipeline)
    {
        out_pipeline = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        const auto vert = this->impl_->shader_modules.find(vertex_shader);
        const auto frag = this->impl_->shader_modules.find(fragment_shader);
        if (dev == this->impl_->devices.end() || layout == this->impl_->pipeline_layouts.end() ||
            vert == this->impl_->shader_modules.end() || frag == this->impl_->shader_modules.end() ||
            !dev->second.create_graphics_pipelines)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // render_pass == 0 means the pipeline targets dynamic rendering (DXVK 2.x): there is no render-pass
        // object; the attachment formats come in via VkPipelineRenderingCreateInfo and viewport/scissor are
        // dynamic. Otherwise resolve the render pass and bake a static viewport as before.
        const bool dynamic_rendering = (render_pass == 0);
        VkRenderPass rp_handle = VK_NULL_HANDLE;
        if (!dynamic_rendering)
        {
            const auto rp = this->impl_->render_passes.find(render_pass);
            if (rp == this->impl_->render_passes.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            rp_handle = rp->second.handle;
        }

        // Rebuild a VkSpecializationInfo per stage from the forwarded entries/data. The map-entry vectors and
        // the two info structs must outlive the vkCreateGraphicsPipelines call below.
        std::array<std::vector<VkSpecializationMapEntry>, 2> spec_map_entries;
        std::array<VkSpecializationInfo, 2> spec_infos{};
        const auto build_spec = [&](const specialization& spec, size_t idx) -> const VkSpecializationInfo* {
            if (spec.entries.empty() || spec.data.empty())
            {
                return nullptr;
            }
            auto& entries = spec_map_entries[idx];
            entries.reserve(spec.entries.size());
            for (const spec_entry& e : spec.entries)
            {
                entries.push_back({.constantID = e.constant_id, .offset = e.offset, .size = e.size});
            }
            spec_infos[idx].mapEntryCount = static_cast<uint32_t>(entries.size());
            spec_infos[idx].pMapEntries = entries.data();
            spec_infos[idx].dataSize = spec.data.size();
            spec_infos[idx].pData = spec.data.data();
            return &spec_infos[idx];
        };

        const specialization fs_effective = fs_spec;

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert->second.handle;
        stages[0].pName = "main";
        stages[0].pSpecializationInfo = build_spec(vs_spec, 0);
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag->second.handle;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = build_spec(fs_effective, 1);

        std::vector<VkVertexInputBindingDescription> vk_bindings;
        vk_bindings.reserve(bindings.size());
        for (const vertex_binding& b : bindings)
        {
            vk_bindings.push_back({.binding = b.binding, .stride = b.stride, .inputRate = static_cast<VkVertexInputRate>(b.input_rate)});
        }

        std::vector<VkVertexInputAttributeDescription> vk_attributes;
        vk_attributes.reserve(attributes.size());
        for (const vertex_attribute& a : attributes)
        {
            vk_attributes.push_back(
                {.location = a.location, .binding = a.binding, .format = static_cast<VkFormat>(a.format), .offset = a.offset});
        }

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(vk_bindings.size());
        vertex_input.pVertexBindingDescriptions = vk_bindings.empty() ? nullptr : vk_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attributes.size());
        vertex_input.pVertexAttributeDescriptions = vk_attributes.empty() ? nullptr : vk_attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = {.width = width, .height = height};

        // Dynamic rendering pipelines use dynamic viewport/scissor (the static width/height is meaningless,
        // DXVK sets them per-draw); a render-pass pipeline keeps the baked viewport from width/height.
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.pViewports = dynamic_rendering ? nullptr : &viewport;
        viewport_state.scissorCount = 1;
        viewport_state.pScissors = dynamic_rendering ? nullptr : &scissor;

        // Use the dynamic-state list DXVK declared on the pipeline. It marks vertex-binding stride, cull,
        // topology, depth/stencil etc. dynamic and sets them at draw time; if the pipeline doesn't declare
        // them dynamic the baked defaults (e.g. a 0 vertex stride) win and produce degenerate geometry.
        std::vector<VkDynamicState> dynamic_state_list;
        dynamic_state_list.reserve(dynamic_states.size() + 2);
        for (const uint32_t s : dynamic_states)
        {
            dynamic_state_list.push_back(static_cast<VkDynamicState>(s));
        }
        if (dynamic_state_list.empty())
        {
            dynamic_state_list.push_back(VK_DYNAMIC_STATE_VIEWPORT);
            dynamic_state_list.push_back(VK_DYNAMIC_STATE_SCISSOR);
        }
        VkPipelineDynamicStateCreateInfo dynamic_state{};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_state_list.size());
        dynamic_state.pDynamicStates = dynamic_state_list.data();

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // The pipeline's sample count must match the render target it is used with; DXVK renders the menu
        // into a multisampled backbuffer, so a hardcoded 1x here would mismatch and drop every fragment.
        multisample.rasterizationSamples =
            rasterization_samples ? static_cast<VkSampleCountFlagBits>(rasterization_samples) : VK_SAMPLE_COUNT_1_BIT;
        // One blend attachment per color target (a render-pass pipeline keeps the single-attachment default).
        const uint32_t color_count = dynamic_rendering ? static_cast<uint32_t>(color_formats.size()) : 1;
        VkPipelineColorBlendAttachmentState blend_template{};
        blend_template.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(color_count, blend_template);
        // Honor the per-attachment blend state DXVK baked into the pipeline. Falls back to the opaque default
        // for any attachment the guest did not describe.
        for (uint32_t i = 0; i < color_count && i < blend_attachments_in.size(); ++i)
        {
            const color_blend_attachment& src = blend_attachments_in[i];
            VkPipelineColorBlendAttachmentState& dst = blend_attachments[i];
            dst.blendEnable = src.blend_enable ? VK_TRUE : VK_FALSE;
            dst.srcColorBlendFactor = static_cast<VkBlendFactor>(src.src_color_blend_factor);
            dst.dstColorBlendFactor = static_cast<VkBlendFactor>(src.dst_color_blend_factor);
            dst.colorBlendOp = static_cast<VkBlendOp>(src.color_blend_op);
            dst.srcAlphaBlendFactor = static_cast<VkBlendFactor>(src.src_alpha_blend_factor);
            dst.dstAlphaBlendFactor = static_cast<VkBlendFactor>(src.dst_alpha_blend_factor);
            dst.alphaBlendOp = static_cast<VkBlendOp>(src.alpha_blend_op);
            dst.colorWriteMask = static_cast<VkColorComponentFlags>(src.color_write_mask);
        }

        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = color_count;
        color_blend.pAttachments = blend_attachments.empty() ? nullptr : blend_attachments.data();

        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = depth.test_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = depth.write_enable ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = static_cast<VkCompareOp>(depth.compare_op);
        std::vector<VkFormat> vk_color_formats;
        VkPipelineRenderingCreateInfo rendering_info{};
        if (dynamic_rendering)
        {
            vk_color_formats.reserve(color_formats.size());
            for (const uint32_t format : color_formats)
            {
                vk_color_formats.push_back(static_cast<VkFormat>(format));
            }
            rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering_info.colorAttachmentCount = static_cast<uint32_t>(vk_color_formats.size());
            rendering_info.pColorAttachmentFormats = vk_color_formats.empty() ? nullptr : vk_color_formats.data();
            rendering_info.depthAttachmentFormat = static_cast<VkFormat>(depth_format);
            rendering_info.stencilAttachmentFormat = static_cast<VkFormat>(stencil_format);
        }

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = dynamic_rendering ? &rendering_info : nullptr;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &color_blend;
        info.pDepthStencilState = depth.test_enable ? &depth_stencil : nullptr;
        info.pDynamicState = dynamic_rendering ? &dynamic_state : nullptr;
        info.layout = layout->second.handle;
        info.renderPass = rp_handle;
        info.subpass = 0;

        VkPipeline pipeline{};
        const VkResult result = dev->second.create_graphics_pipelines(dev->second.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->pipelines.emplace(id, impl::pipeline_data{.handle = pipeline, .device_id = device});
        out_pipeline = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_compute_pipeline(uint64_t device, uint64_t pipeline_layout, uint64_t shader_module, uint64_t& out_pipeline)
    {
        out_pipeline = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        const auto shader = this->impl_->shader_modules.find(shader_module);
        if (dev == this->impl_->devices.end() || layout == this->impl_->pipeline_layouts.end() ||
            shader == this->impl_->shader_modules.end() || !dev->second.create_compute_pipelines)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.layout = layout->second.handle;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader->second.handle;
        info.stage.pName = "main";

        VkPipeline pipeline{};
        const VkResult result = dev->second.create_compute_pipelines(dev->second.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->next_id++;
        this->impl_->pipelines.emplace(id, impl::pipeline_data{.handle = pipeline, .device_id = device});
        out_pipeline = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline(uint64_t device, uint64_t pipeline)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipelines.find(pipeline);
        if (it == this->impl_->pipelines.end())
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_pipeline)
        {
            dev->second.destroy_pipeline(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->pipelines.erase(it);
    }

    int32_t vulkan_host::cmd_begin_render_pass(uint64_t command_buffer, uint64_t render_pass, uint64_t framebuffer, uint32_t width,
                                               uint32_t height, float r, float g, float b, float a, float clear_depth)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto rp = this->impl_->render_passes.find(render_pass);
        const auto fb = this->impl_->framebuffers.find(framebuffer);
        if (cb == this->impl_->command_buffers.end() || rp == this->impl_->render_passes.end() || fb == this->impl_->framebuffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_begin_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // clearValues are indexed by attachment: [0] color, [1] depth (present only when the render pass
        // was created with a depth attachment).
        std::array<VkClearValue, 2> clears{};
        clears[0].color.float32[0] = r;
        clears[0].color.float32[1] = g;
        clears[0].color.float32[2] = b;
        clears[0].color.float32[3] = a;
        clears[1].depthStencil = {.depth = clear_depth, .stencil = 0};

        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = rp->second.handle;
        info.framebuffer = fb->second.handle;
        info.renderArea.extent = {.width = width, .height = height};
        info.clearValueCount = rp->second.has_depth ? 2 : 1;
        info.pClearValues = clears.data();

        dev->second.cmd_begin_render_pass(cb->second.handle, &info, VK_SUBPASS_CONTENTS_INLINE);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_pipeline(uint64_t command_buffer, uint64_t pipeline, uint32_t bind_point)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto pipe = this->impl_->pipelines.find(pipeline);
        if (cb == this->impl_->command_buffers.end() || pipe == this->impl_->pipelines.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_pipeline)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_bind_pipeline(cb->second.handle, static_cast<VkPipelineBindPoint>(bind_point), pipe->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_dispatch(uint64_t command_buffer, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_dispatch)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_dispatch(cb->second.handle, group_count_x, group_count_y, group_count_z);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_dispatch_indirect(uint64_t command_buffer, uint64_t buffer, uint64_t offset)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_dispatch_indirect)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_dispatch_indirect(cb->second.handle, buf->second.handle, offset);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw(uint64_t command_buffer, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                                  uint32_t first_instance)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_draw(cb->second.handle, vertex_count, instance_count, first_vertex, first_instance);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_vertex_buffers(uint64_t command_buffer, uint32_t first_binding, uint32_t count,
                                                 const uint64_t* buffer_ids, const uint64_t* offsets)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_vertex_buffers)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkBuffer> handles(count);
        std::vector<VkDeviceSize> vk_offsets(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (buffer_ids[i] == 0)
            {
                handles[i] = VK_NULL_HANDLE; // null vertex buffer (nullDescriptor)
                vk_offsets[i] = 0;
                continue;
            }
            const auto buf = this->impl_->buffers.find(buffer_ids[i]);
            if (buf == this->impl_->buffers.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles[i] = buf->second.handle;
            vk_offsets[i] = offsets[i];
        }

        dev->second.cmd_bind_vertex_buffers(cb->second.handle, first_binding, count, handles.data(), vk_offsets.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_vertex_buffers2(uint64_t command_buffer, uint32_t first_binding, uint32_t count,
                                                  const uint64_t* buffer_ids, const uint64_t* offsets, const uint64_t* sizes,
                                                  const uint64_t* strides)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_vertex_buffers2)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkBuffer> handles(count);
        std::vector<VkDeviceSize> vk_offsets(count);
        std::vector<VkDeviceSize> vk_sizes(count);
        std::vector<VkDeviceSize> vk_strides(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (buffer_ids[i] == 0)
            {
                // Null vertex buffer (VK_EXT_robustness2 nullDescriptor): DXVK binds VK_NULL_HANDLE for
                // unused vertex-input slots. The matching offset/size must be 0.
                handles[i] = VK_NULL_HANDLE;
                vk_offsets[i] = 0;
                vk_sizes[i] = 0;
                vk_strides[i] = strides ? strides[i] : 0;
                continue;
            }
            const auto buf = this->impl_->buffers.find(buffer_ids[i]);
            if (buf == this->impl_->buffers.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles[i] = buf->second.handle;
            vk_offsets[i] = offsets[i];
            vk_sizes[i] = sizes ? sizes[i] : VK_WHOLE_SIZE;
            vk_strides[i] = strides ? strides[i] : 0;
        }

        dev->second.cmd_bind_vertex_buffers2(cb->second.handle, first_binding, count, handles.data(), vk_offsets.data(),
                                             sizes ? vk_sizes.data() : nullptr, strides ? vk_strides.data() : nullptr);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_index_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint32_t index_type)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_index_buffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_bind_index_buffer(cb->second.handle, buf->second.handle, offset, static_cast<VkIndexType>(index_type));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw_indexed(uint64_t command_buffer, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                          int32_t vertex_offset, uint32_t first_instance)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indexed)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_draw_indexed(cb->second.handle, index_count, instance_count, first_index, vertex_offset, first_instance);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_descriptor_sets(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t first_set,
                                                  std::span<const uint64_t> sets, uint32_t bind_point,
                                                  std::span<const uint32_t> dynamic_offsets)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSet> handles;
        handles.reserve(sets.size());
        for (const uint64_t id : sets)
        {
            const auto set = this->impl_->descriptor_sets.find(id);
            if (set == this->impl_->descriptor_sets.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles.push_back(set->second.handle);
        }

        dev->second.cmd_bind_descriptor_sets(cb->second.handle, static_cast<VkPipelineBindPoint>(bind_point), layout->second.handle,
                                             first_set, static_cast<uint32_t>(handles.size()), handles.data(),
                                             static_cast<uint32_t>(dynamic_offsets.size()), dynamic_offsets.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_end_render_pass(uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_end_render_pass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_end_render_pass(cb->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_begin_rendering(uint64_t command_buffer, int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h,
                                             uint32_t layer_count, uint32_t view_mask, uint32_t flags,
                                             std::span<const rendering_attachment> color, const rendering_attachment* depth,
                                             const rendering_attachment* stencil)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_begin_rendering)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto view_handle = [&](uint64_t id) -> VkImageView {
            if (id == 0)
            {
                return VK_NULL_HANDLE;
            }
            const auto it = this->impl_->image_views.find(id);
            return it != this->impl_->image_views.end() ? it->second.handle : VK_NULL_HANDLE;
        };
        const auto build = [&](const rendering_attachment& a) {
            VkRenderingAttachmentInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = view_handle(a.image_view);
            info.imageLayout = static_cast<VkImageLayout>(a.image_layout);
            info.resolveMode = static_cast<VkResolveModeFlagBits>(a.resolve_mode);
            info.resolveImageView = view_handle(a.resolve_image_view);
            info.resolveImageLayout = static_cast<VkImageLayout>(a.resolve_image_layout);
            info.loadOp = static_cast<VkAttachmentLoadOp>(a.load_op);
            info.storeOp = static_cast<VkAttachmentStoreOp>(a.store_op);
            static_assert(sizeof(info.clearValue) == sizeof(a.clear_value));
            std::memcpy(&info.clearValue, a.clear_value.data(), sizeof(info.clearValue));
            return info;
        };

        std::vector<VkRenderingAttachmentInfo> color_infos;
        color_infos.reserve(color.size());
        for (const auto& a : color)
        {
            color_infos.push_back(build(a));
        }

        // Diagnostic: force every colour attachment to clear to bright green. If a presented frame is then
        // uniform green the draws produce no fragments (rasterisation/geometry); if it shows black shapes on
        // green the geometry rasterises but the shading is black (shader/texture).
        VkRenderingAttachmentInfo depth_info{};
        VkRenderingAttachmentInfo stencil_info{};
        if (depth)
        {
            depth_info = build(*depth);
        }
        if (stencil)
        {
            stencil_info = build(*stencil);
        }

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.flags = flags;
        info.renderArea.offset = {.x = area_x, .y = area_y};
        info.renderArea.extent = {.width = area_w, .height = area_h};
        info.layerCount = layer_count;
        info.viewMask = view_mask;
        info.colorAttachmentCount = static_cast<uint32_t>(color_infos.size());
        info.pColorAttachments = color_infos.empty() ? nullptr : color_infos.data();
        info.pDepthAttachment = depth ? &depth_info : nullptr;
        info.pStencilAttachment = stencil ? &stencil_info : nullptr;

        dev->second.cmd_begin_rendering(cb->second.handle, &info);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_end_rendering(uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_end_rendering)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_end_rendering(cb->second.handle);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_push_constants(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t stage_flags, uint32_t offset,
                                            uint32_t size, const void* data)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_push_constants)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_push_constants(cb->second.handle, layout->second.handle, stage_flags, offset, size, data);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_viewport(uint64_t command_buffer, uint32_t first, bool with_count,
                                          std::span<const viewport_entry> viewports)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkViewport> entries(viewports.size());
        for (size_t i = 0; i < viewports.size(); ++i)
        {
            entries[i] = {
                .x = viewports[i].x,
                .y = viewports[i].y,
                .width = viewports[i].width,
                .height = viewports[i].height,
                .minDepth = viewports[i].min_depth,
                .maxDepth = viewports[i].max_depth,
            };
        }
        if (with_count)
        {
            if (!dev->second.cmd_set_viewport_with_count)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_viewport_with_count(cb->second.handle, static_cast<uint32_t>(entries.size()), entries.data());
        }
        else
        {
            if (!dev->second.cmd_set_viewport)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_viewport(cb->second.handle, first, static_cast<uint32_t>(entries.size()), entries.data());
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_scissor(uint64_t command_buffer, uint32_t first, bool with_count, std::span<const scissor_entry> scissors)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkRect2D> entries(scissors.size());
        for (size_t i = 0; i < scissors.size(); ++i)
        {
            entries[i] = {
                .offset = {.x = scissors[i].offset_x, .y = scissors[i].offset_y},
                .extent = {.width = scissors[i].width, .height = scissors[i].height},
            };
        }

        if (with_count)
        {
            if (!dev->second.cmd_set_scissor_with_count)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_scissor_with_count(cb->second.handle, static_cast<uint32_t>(entries.size()), entries.data());
        }
        else
        {
            if (!dev->second.cmd_set_scissor)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_scissor(cb->second.handle, first, static_cast<uint32_t>(entries.size()), entries.data());
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_depth_bias(uint64_t command_buffer, float constant_factor, float clamp, float slope_factor)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_depth_bias)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        // The guest (DXVK) can submit a non-finite depth-bias constant for passes without a depth
        // attachment. Vulkan treats NaN/Inf bias as undefined; NVIDIA biases every fragment's depth to
        // NaN, which fails the implicit 0<=z<=1 range test and discards all fragments. Clamp to 0 so the
        // bias becomes a no-op instead of silently killing the draw.
        const auto sanitize = [](float v) { return std::isfinite(v) ? v : 0.0f; };
        dev->second.cmd_set_depth_bias(cb->second.handle, sanitize(constant_factor), sanitize(clamp), sanitize(slope_factor));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_blend_constants(uint64_t command_buffer, const std::array<float, 4>& constants)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_blend_constants)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_set_blend_constants(cb->second.handle, constants.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_depth_bounds(uint64_t command_buffer, float min_depth_bounds, float max_depth_bounds)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_depth_bounds)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_set_depth_bounds(cb->second.handle, min_depth_bounds, max_depth_bounds);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_line_width(uint64_t command_buffer, float line_width)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_line_width)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_set_line_width(cb->second.handle, line_width);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_stencil(uint64_t command_buffer, uint32_t which, uint32_t face_mask, uint32_t value)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto face = static_cast<VkStencilFaceFlags>(face_mask);
        switch (static_cast<gpu_bridge::stencil_dynamic_state>(which))
        {
        case gpu_bridge::stencil_dynamic_state::compare_mask:
            if (!dev->second.cmd_set_stencil_compare_mask)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_stencil_compare_mask(cb->second.handle, face, value);
            break;
        case gpu_bridge::stencil_dynamic_state::write_mask:
            if (!dev->second.cmd_set_stencil_write_mask)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_stencil_write_mask(cb->second.handle, face, value);
            break;
        case gpu_bridge::stencil_dynamic_state::reference:
            if (!dev->second.cmd_set_stencil_reference)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_stencil_reference(cb->second.handle, face, value);
            break;
        default:
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_stencil_op(uint64_t command_buffer, uint32_t face_mask, uint32_t fail_op, uint32_t pass_op,
                                            uint32_t depth_fail_op, uint32_t compare_op)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_stencil_op)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_set_stencil_op(cb->second.handle, static_cast<VkStencilFaceFlags>(face_mask), static_cast<VkStencilOp>(fail_op),
                                       static_cast<VkStencilOp>(pass_op), static_cast<VkStencilOp>(depth_fail_op),
                                       static_cast<VkCompareOp>(compare_op));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_set_dynamic_u32(uint64_t command_buffer, uint32_t state, uint32_t value)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const VkCommandBuffer handle = cb->second.handle;
        switch (static_cast<gpu_bridge::dynamic_state_u32>(state))
        {
        case gpu_bridge::dynamic_state_u32::cull_mode:
            if (!dev->second.cmd_set_cull_mode)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_cull_mode(handle, static_cast<VkCullModeFlags>(value));
            break;
        case gpu_bridge::dynamic_state_u32::front_face:
            if (!dev->second.cmd_set_front_face)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_front_face(handle, static_cast<VkFrontFace>(value));
            break;
        case gpu_bridge::dynamic_state_u32::primitive_topology:
            if (!dev->second.cmd_set_primitive_topology)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_primitive_topology(handle, static_cast<VkPrimitiveTopology>(value));
            break;
        case gpu_bridge::dynamic_state_u32::depth_test_enable:
            if (!dev->second.cmd_set_depth_test_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_test_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::depth_write_enable:
            if (!dev->second.cmd_set_depth_write_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_write_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::depth_compare_op:
            if (!dev->second.cmd_set_depth_compare_op)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_compare_op(handle, static_cast<VkCompareOp>(value));
            break;
        case gpu_bridge::dynamic_state_u32::depth_bounds_test_enable:
            if (!dev->second.cmd_set_depth_bounds_test_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_bounds_test_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::stencil_test_enable:
            if (!dev->second.cmd_set_stencil_test_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_stencil_test_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::rasterizer_discard_enable:
            if (!dev->second.cmd_set_rasterizer_discard_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_rasterizer_discard_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::depth_bias_enable:
            if (!dev->second.cmd_set_depth_bias_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_bias_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::primitive_restart_enable:
            if (!dev->second.cmd_set_primitive_restart_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_primitive_restart_enable(handle, value);
            break;
        default:
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return VK_SUCCESS;
    }
}
