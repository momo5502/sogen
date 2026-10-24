#include "vulkan_host.hpp"
#include "../vulkan_handle_utils.hpp"
#include "spirv_clip_distance.hpp"
#include "vulkan_format_block.hpp"

#include <address_utils.hpp>
#include <utils/finally.hpp>
#include <utils/serialization.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <chrono>
#include <vector>
#include <ranges>

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
        // Extensions whose entry points the bridge does not marshal. They must never reach the guest: a guest
        // that sees them enables them and then calls into nothing. DXVK, for instance, creates shared textures
        // as soon as it sees VK_KHR_external_memory_win32 and crashes when the shim has no implementation.
        // HANDLE-based interop cannot work across the bridge anyway - guest handles are emulator objects.
        constexpr std::array unsupported_device_extensions{
            std::string_view{"VK_KHR_external_memory_win32"},    //
            std::string_view{"VK_KHR_external_semaphore_win32"}, //
            std::string_view{"VK_KHR_external_fence_win32"},     //
            std::string_view{"VK_KHR_win32_keyed_mutex"},        //
            std::string_view{"VK_EXT_full_screen_exclusive"},    //
            std::string_view{"VK_EXT_swapchain_maintenance1"},   //
            std::string_view{"VK_KHR_swapchain_mutable_format"}, //
            std::string_view{"VK_KHR_present_id"},               //
            std::string_view{"VK_KHR_present_wait"},             //
            std::string_view{"VK_GOOGLE_display_timing"},        //
            std::string_view{"VK_EXT_hdr_metadata"},             //
            std::string_view{"VK_NV_low_latency2"},              //
        };

        bool is_unsupported_extension_name(const std::string_view name)
        {
            return std::ranges::find(unsupported_device_extensions, name) != unsupported_device_extensions.end();
        }

        bool is_unsupported_device_extension(const VkExtensionProperties& extension)
        {
            return is_unsupported_extension_name(std::string_view{static_cast<const char*>(extension.extensionName)});
        }

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

        // Validates a guest-controlled [offset, offset + size) range against an allocation size without
        // overflowing. VK_WHOLE_SIZE is only accepted where the Vulkan API itself permits it (flush /
        // invalidate of mapped ranges), in which case it covers the rest of the allocation from offset.
        bool is_memory_range_valid(const uint64_t allocation_size, const uint64_t offset, const uint64_t size, const bool allow_whole_size)
        {
            if (offset > allocation_size)
            {
                return false;
            }

            if (allow_whole_size && size == VK_WHOLE_SIZE)
            {
                return true;
            }

            return size <= allocation_size - offset;
        }

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

    // A sampler's description holds an optional pair, which is not trivially copyable.
    static void serialize(utils::buffer_serializer& buffer, const vulkan_host::sampler_desc& desc)
    {
        buffer.write(desc.flags);
        buffer.write(desc.mag_filter);
        buffer.write(desc.min_filter);
        buffer.write(desc.address_mode_u);
        buffer.write(desc.address_mode_v);
        buffer.write(desc.address_mode_w);
        buffer.write(desc.mipmap_mode);
        buffer.write(desc.compare_enable);
        buffer.write(desc.compare_op);
        buffer.write(desc.anisotropy_enable);
        buffer.write(desc.border_color);
        buffer.write(desc.mip_lod_bias);
        buffer.write(desc.max_anisotropy);
        buffer.write(desc.min_lod);
        buffer.write(desc.max_lod);
        buffer.write(desc.unnormalized_coordinates);
        buffer.write_optional(desc.reduction_mode);
        buffer.write(desc.custom_border_color.has_value());
        if (desc.custom_border_color)
        {
            buffer.write(desc.custom_border_color->first);
            buffer.write(desc.custom_border_color->second);
        }
    }

    static void deserialize(utils::buffer_deserializer& buffer, vulkan_host::sampler_desc& desc)
    {
        buffer.read(desc.flags);
        buffer.read(desc.mag_filter);
        buffer.read(desc.min_filter);
        buffer.read(desc.address_mode_u);
        buffer.read(desc.address_mode_v);
        buffer.read(desc.address_mode_w);
        buffer.read(desc.mipmap_mode);
        buffer.read(desc.compare_enable);
        buffer.read(desc.compare_op);
        buffer.read(desc.anisotropy_enable);
        buffer.read(desc.border_color);
        buffer.read(desc.mip_lod_bias);
        buffer.read(desc.max_anisotropy);
        buffer.read(desc.min_lod);
        buffer.read(desc.max_lod);
        buffer.read(desc.unnormalized_coordinates);
        buffer.read_optional(desc.reduction_mode);
        desc.custom_border_color.reset();
        if (buffer.read<bool>())
        {
            auto& border = desc.custom_border_color.emplace();
            buffer.read(border.first);
            buffer.read(border.second);
        }
    }

    namespace
    {
        template <typename T>
        class object_table : public std::unordered_map<uint64_t, T>
        {
          public:
            explicit object_table(std::unordered_set<uint64_t>* const* references)
                : references_(references)
            {
            }

            auto find(const uint64_t id)
            {
                this->note(id);
                return std::unordered_map<uint64_t, T>::find(id);
            }

            auto find(const uint64_t id) const
            {
                this->note(id);
                return std::unordered_map<uint64_t, T>::find(id);
            }

          private:
            std::unordered_set<uint64_t>* const* references_{};

            void note(const uint64_t id) const
            {
                if (*this->references_)
                {
                    (*this->references_)->insert(id);
                }
            }
        };
    }

    struct vulkan_host::impl
    {
        // The command buffer whose recorded command is running, which notes every object looked up.
        std::unordered_set<uint64_t>* recording_references{};

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
            PFN_vkGetPhysicalDeviceImageFormatProperties2 get_physical_device_image_format_properties2{};
            PFN_vkGetPhysicalDeviceExternalBufferProperties get_physical_device_external_buffer_properties{};
            PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties{};
            PFN_vkGetPhysicalDeviceQueueFamilyProperties2 get_queue_family_properties2{};
            PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties{};
            PFN_vkGetPhysicalDeviceMemoryProperties2 get_physical_device_memory_properties2{};
            PFN_vkGetPhysicalDeviceFeatures2 get_physical_device_features2{};
            PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2{};
            PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extension_properties{};
            PFN_vkDestroySurfaceKHR destroy_surface{};
            PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support{};
            PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities{};
            PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_surface_formats{};
            PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_surface_present_modes{};
            PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get_cooperative_matrix_properties{};
            PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR get_fragment_shading_rates{};
            PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR get_calibrateable_time_domains{};
            PFN_vkCreateDevice create_device{};
            PFN_vkGetDeviceProcAddr get_device_proc_addr{};
            std::vector<std::string> extensions{};
        };

        struct physical_device_data
        {
            VkPhysicalDevice handle{};
            uint64_t instance_id{};
            std::optional<bool> portability{};
        };

        static bool has_device_extension(const instance_data& instance, VkPhysicalDevice device, const std::string_view name)
        {
            if (!instance.enumerate_device_extension_properties)
            {
                return false;
            }

            uint32_t count = 0;
            VkResult result = instance.enumerate_device_extension_properties(device, nullptr, &count, nullptr);
            if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            {
                return false;
            }

            std::vector<VkExtensionProperties> extensions(count);
            if (count > 0)
            {
                result = instance.enumerate_device_extension_properties(device, nullptr, &count, extensions.data());
                if (result != VK_SUCCESS && result != VK_INCOMPLETE)
                {
                    return false;
                }
            }

            return std::ranges::any_of(extensions, [&](const VkExtensionProperties& extension) {
                return std::string_view{static_cast<const char*>(extension.extensionName)} == name;
            });
        }

        // VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME lives in vulkan_beta.h behind VK_ENABLE_BETA_EXTENSIONS.
        static constexpr std::string_view portability_subset_extension_name = "VK_KHR_portability_subset";

        static bool is_portability_device(const instance_data& instance, physical_device_data& device)
        {
            if (!device.portability)
            {
                device.portability = has_device_extension(instance, device.handle, portability_subset_extension_name);
            }

            return *device.portability;
        }

        struct device_create_params
        {
            uint64_t physical_device{};
            std::vector<gpu_bridge::device_queue_create_entry> queues{};
            std::vector<std::byte> extensions{};
            uint32_t extension_count{};
            std::vector<std::byte> features{};
            uint32_t feature_count{};
            bool spoof_missing_features{};
        };

        struct device_data
        {
            VkDevice handle{};
            device_create_params create{};
            std::vector<VkFence> abandoned_drain_fences{};
            bool maintenance5_enabled{};
            // Held shared by presents running without the caller's locks, so destroying the device waits for them.
            std::shared_ptr<std::shared_mutex> presents = std::make_shared<std::shared_mutex>();
            uint64_t instance_id{};
            VkPhysicalDevice physical_device{}; // the device this was created from (for memory queries)
            uint32_t queue_family_index{};      // the single family this device was created with
            // The driver lacks these features although the guest was told otherwise; shaders are rewritten.
            bool strip_clip_distance{};
            bool strip_cull_distance{};
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
            PFN_vkCreateSwapchainKHR create_swapchain{};
            PFN_vkDestroySwapchainKHR destroy_swapchain{};
            PFN_vkGetSwapchainImagesKHR get_swapchain_images{};
            PFN_vkAcquireNextImageKHR acquire_next_image{};
            PFN_vkQueuePresentKHR queue_present{};
            PFN_vkAllocateMemory allocate_memory{};
            PFN_vkFreeMemory free_memory{};
            PFN_vkGetDeviceMemoryCommitment get_device_memory_commitment{};
            PFN_vkGetCalibratedTimestampsKHR get_calibrated_timestamps{};
            PFN_vkMapMemory map_memory{};
            PFN_vkUnmapMemory unmap_memory{};
            PFN_vkMapMemory2KHR map_memory2{};
            PFN_vkUnmapMemory2KHR unmap_memory2{};
            uint64_t placed_map_alignment{};
            PFN_vkGetMemoryHostPointerPropertiesEXT get_memory_host_pointer_properties{};
            uint64_t host_import_alignment{};
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
            PFN_vkCmdClearAttachments cmd_clear_attachments{};
            PFN_vkCmdClearDepthStencilImage cmd_clear_depth_stencil_image{};
            PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};
            PFN_vkCmdResolveImage cmd_resolve_image{};
            PFN_vkCmdUpdateBuffer cmd_update_buffer{};
            PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image{};
            PFN_vkCmdCopyImage cmd_copy_image{};
            PFN_vkCmdBlitImage cmd_blit_image{};
            PFN_vkCmdCopyBuffer cmd_copy_buffer{};
            PFN_vkCreateSampler create_sampler{};
            PFN_vkDestroySampler destroy_sampler{};
            PFN_vkCreateShaderModule create_shader_module{};
            PFN_vkDestroyShaderModule destroy_shader_module{};
            PFN_vkGetShaderModuleIdentifierEXT get_shader_module_identifier{};
            PFN_vkGetShaderModuleCreateInfoIdentifierEXT get_shader_module_create_info_identifier{};
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
            PFN_vkCmdBeginQueryIndexedEXT cmd_begin_query_indexed{};
            PFN_vkCmdEndQueryIndexedEXT cmd_end_query_indexed{};
            PFN_vkCmdBindTransformFeedbackBuffersEXT cmd_bind_transform_feedback_buffers{};
            PFN_vkCmdBeginTransformFeedbackEXT cmd_begin_transform_feedback{};
            PFN_vkCmdEndTransformFeedbackEXT cmd_end_transform_feedback{};
            PFN_vkCmdDrawIndirectByteCountEXT cmd_draw_indirect_byte_count{};
            PFN_vkCmdWriteTimestamp cmd_write_timestamp{};
            PFN_vkCmdWriteTimestamp2 cmd_write_timestamp2{};
            PFN_vkCmdCopyQueryPoolResults cmd_copy_query_pool_results{};
            PFN_vkCreateRenderPass create_render_pass{};
            PFN_vkDestroyRenderPass destroy_render_pass{};
            PFN_vkCreateFramebuffer create_framebuffer{};
            PFN_vkDestroyFramebuffer destroy_framebuffer{};
            PFN_vkCreatePipelineLayout create_pipeline_layout{};
            PFN_vkDestroyPipelineLayout destroy_pipeline_layout{};
            PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout{};
            PFN_vkGetDescriptorSetLayoutSupport get_descriptor_set_layout_support{};
            PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout{};
            PFN_vkCreateDescriptorPool create_descriptor_pool{};
            PFN_vkDestroyDescriptorPool destroy_descriptor_pool{};
            PFN_vkResetDescriptorPool reset_descriptor_pool{};
            PFN_vkAllocateDescriptorSets allocate_descriptor_sets{};
            PFN_vkFreeDescriptorSets free_descriptor_sets{};
            PFN_vkUpdateDescriptorSets update_descriptor_sets{};
            PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets{};
            PFN_vkCreatePipelineCache create_pipeline_cache{};
            PFN_vkDestroyPipelineCache destroy_pipeline_cache{};
            PFN_vkGetPipelineCacheData get_pipeline_cache_data{};
            PFN_vkMergePipelineCaches merge_pipeline_caches{};
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
            PFN_vkCmdBindIndexBuffer2KHR cmd_bind_index_buffer2{};
            PFN_vkCmdDrawIndexed cmd_draw_indexed{};
            PFN_vkCmdDrawIndexedIndirect cmd_draw_indexed_indirect{};
            PFN_vkCmdDrawIndexedIndirectCount cmd_draw_indexed_indirect_count{};
            PFN_vkCmdDrawIndirect cmd_draw_indirect{};
            PFN_vkCmdDrawIndirectCount cmd_draw_indirect_count{};
            PFN_vkCmdEndRenderPass cmd_end_render_pass{};
            PFN_vkCmdNextSubpass cmd_next_subpass{};
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
            PFN_vkCmdSetDepthClipEnableEXT cmd_set_depth_clip_enable{};
            PFN_vkCmdSetStencilTestEnable cmd_set_stencil_test_enable{};
            PFN_vkCmdSetRasterizerDiscardEnable cmd_set_rasterizer_discard_enable{};
            PFN_vkCmdSetDepthBiasEnable cmd_set_depth_bias_enable{};
            PFN_vkCmdSetPrimitiveRestartEnable cmd_set_primitive_restart_enable{};
        };

        struct surface_data
        {
            VkSurfaceKHR handle{};
            uint64_t instance_id{};
        };

        struct swapchain_params
        {
            uint32_t flags{};
            uint32_t format{};
            uint32_t color_space{};
            uint32_t width{};
            uint32_t height{};
            uint32_t image_array_layers{};
            uint32_t min_image_count{};
            uint32_t image_usage{};
            uint32_t sharing_mode{};
            std::vector<uint32_t> queue_family_indices{};
            uint32_t pre_transform{};
            uint32_t composite_alpha{};
            uint32_t present_mode{};
            uint32_t clipped{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->flags);
                buffer.write(this->format);
                buffer.write(this->color_space);
                buffer.write(this->width);
                buffer.write(this->height);
                buffer.write(this->image_array_layers);
                buffer.write(this->min_image_count);
                buffer.write(this->image_usage);
                buffer.write(this->sharing_mode);
                buffer.write_vector(this->queue_family_indices);
                buffer.write(this->pre_transform);
                buffer.write(this->composite_alpha);
                buffer.write(this->present_mode);
                buffer.write(this->clipped);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->flags);
                buffer.read(this->format);
                buffer.read(this->color_space);
                buffer.read(this->width);
                buffer.read(this->height);
                buffer.read(this->image_array_layers);
                buffer.read(this->min_image_count);
                buffer.read(this->image_usage);
                buffer.read(this->sharing_mode);
                buffer.read_vector(this->queue_family_indices);
                buffer.read(this->pre_transform);
                buffer.read(this->composite_alpha);
                buffer.read(this->present_mode);
                buffer.read(this->clipped);
            }
        };

        struct swapchain_data
        {
            VkSwapchainKHR handle{};
            uint64_t device_id{};
            uint64_t surface_id{};
            std::vector<uint64_t> image_ids{};
            swapchain_params create{};
            // Which images the guest acquired and has not presented yet.
            std::vector<bool> acquired{};
        };

        object_table<instance_data> instances{&this->recording_references};
        object_table<physical_device_data> physical_devices{&this->recording_references};
        std::unordered_map<VkPhysicalDevice, uint64_t> physical_device_ids;

        struct queue_data
        {
            VkQueue handle{};
            uint64_t device_id{};
            uint32_t queue_family_index{};
            uint32_t queue_index{};
        };

        struct command_pool_data
        {
            VkCommandPool handle{};
            uint64_t device_id{};
            uint32_t queue_family_index{};
            uint32_t flags{};
        };

        struct layout_transition
        {
            uint64_t image{};
            uint32_t aspect_mask{};
            uint32_t base_mip_level{};
            uint32_t level_count{};
            uint32_t base_array_layer{};
            uint32_t layer_count{};
            uint32_t new_layout{};
            bool transfers_ownership{};
            // Set instead of the above where a primary executes a secondary command buffer.
            uint64_t secondary{};
        };

        struct command_buffer_data
        {
            VkCommandBuffer handle{};
            uint64_t device_id{};
            uint64_t pool_id{};
            uint32_t level{};
            std::vector<std::byte> recorded{};
            std::vector<int32_t> recorded_results{};
            std::vector<layout_transition> layout_transitions{};
            // Query resets take effect when the command buffer is submitted: pool, first query, count.
            std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> query_resets{};
            // Every object the recorded commands looked up. Ids are never reused, so one that no longer exists, or whose
            // memory no longer does, was destroyed after being recorded and invalidated the command buffer.
            std::unordered_set<uint64_t> references{};
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
            uint32_t flags{};
        };

        struct semaphore_data
        {
            VkSemaphore handle{};
            uint64_t device_id{};
            uint32_t flags{};
            uint32_t type{};
            // Vulkan cannot report these, so they are tracked from submissions for snapshots: whether a binary semaphore
            // has a signal no wait consumed yet, and the highest timeline values signals and waits were submitted for.
            bool binary_signaled{};
            bool binary_wait_unsignaled{};
            uint64_t highest_signal{};
            uint64_t highest_wait{};
        };

        bool is_live(const uint64_t id) const
        {
            const auto bound_memory_live = [&](const uint64_t memory) { return memory == 0 || this->memories.contains(memory); };
            if (const auto buffer = this->buffers.find(id); buffer != this->buffers.end())
            {
                return bound_memory_live(buffer->second.memory_id);
            }

            if (const auto image = this->images.find(id); image != this->images.end())
            {
                return bound_memory_live(image->second.memory_id);
            }

            return this->instances.contains(id) || this->physical_devices.contains(id) || this->devices.contains(id) ||
                   this->queues.contains(id) || this->command_pools.contains(id) || this->command_buffers.contains(id) ||
                   this->fences.contains(id) || this->events.contains(id) || this->semaphores.contains(id) || this->memories.contains(id) ||
                   this->samplers.contains(id) || this->surfaces.contains(id) || this->swapchains.contains(id) ||
                   this->shader_modules.contains(id) || this->image_views.contains(id) || this->buffer_views.contains(id) ||
                   this->query_pools.contains(id) || this->render_passes.contains(id) || this->framebuffers.contains(id) ||
                   this->pipeline_layouts.contains(id) || this->pipelines.contains(id) || this->pipeline_caches.contains(id) ||
                   this->descriptor_set_layouts.contains(id) || this->descriptor_pools.contains(id) || this->descriptor_sets.contains(id);
        }

        void apply_layout_transitions(const VkQueue queue, const uint64_t* submitted, const uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                this->apply_layout_transitions(queue, submitted[i]);
            }
        }

        void apply_layout_transitions(const VkQueue queue, const uint64_t command_buffer)
        {
            const auto cb = this->command_buffers.find(command_buffer);
            if (cb == this->command_buffers.end())
            {
                return;
            }

            for (const auto& [pool, first_query, count] : cb->second.query_resets)
            {
                if (const auto reset = this->query_pools.find(pool); reset != this->query_pools.end())
                {
                    reset->second.forget_restored(first_query, count);
                }
            }

            for (const auto& transition : cb->second.layout_transitions)
            {
                if (transition.secondary != 0)
                {
                    if (transition.secondary != command_buffer)
                    {
                        this->apply_layout_transitions(queue, transition.secondary);
                    }
                    continue;
                }

                {
                    const auto image = this->images.find(transition.image);
                    if (image == this->images.end())
                    {
                        continue;
                    }

                    auto& data = image->second;
                    data.ownership_transferred |= transition.transfers_ownership;
                    data.layout_undefined = false;

                    // Submissions to different queues can run in another order than they were made in, which the tracked
                    // layouts cannot follow.
                    data.used_by_several_queues |= data.last_queue && data.last_queue != queue;
                    data.last_queue = queue;

                    const auto aspects = get_vulkan_format_aspects(data.create.format);
                    const auto mip_end = transition.level_count == VK_REMAINING_MIP_LEVELS
                                             ? data.create.mip_levels
                                             : std::min(data.create.mip_levels, transition.base_mip_level + transition.level_count);
                    const auto layer_end = transition.layer_count == VK_REMAINING_ARRAY_LAYERS
                                               ? data.create.array_layers
                                               : std::min(data.create.array_layers, transition.base_array_layer + transition.layer_count);

                    uint32_t aspect = 0;
                    for (uint32_t bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
                    {
                        if ((aspects & bit) == 0)
                        {
                            continue;
                        }

                        if ((transition.aspect_mask & bit) != 0)
                        {
                            for (uint32_t mip = transition.base_mip_level; mip < mip_end; ++mip)
                            {
                                for (uint32_t layer = transition.base_array_layer; layer < layer_end; ++layer)
                                {
                                    data.layouts[data.subresource_index(aspect, mip, layer)] = transition.new_layout;
                                }
                            }
                        }

                        ++aspect;
                    }
                }
            }
        }

        bool image_usage_supported(const device_data& dev, const VkImageCreateInfo& info)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_physical_device_image_format_properties)
            {
                return false;
            }

            VkImageFormatProperties properties{};
            return instance->second.get_physical_device_image_format_properties(
                       dev.physical_device, info.format, info.imageType, info.tiling, info.usage, info.flags, &properties) == VK_SUCCESS;
        }

        template <typename Entry>
        void track_submitted_semaphores(const Entry* waits, const uint32_t wait_count, const Entry* signals, const uint32_t signal_count)
        {
            for (uint32_t i = 0; i < wait_count; ++i)
            {
                if (const auto it = this->semaphores.find(waits[i].semaphore); it != this->semaphores.end())
                {
                    if (it->second.type != VK_SEMAPHORE_TYPE_TIMELINE && !it->second.binary_signaled)
                    {
                        it->second.binary_wait_unsignaled = true;
                    }
                    it->second.binary_signaled = false;
                    it->second.highest_wait = std::max(it->second.highest_wait, waits[i].value);
                }
            }

            for (uint32_t i = 0; i < signal_count; ++i)
            {
                if (const auto it = this->semaphores.find(signals[i].semaphore); it != this->semaphores.end())
                {
                    it->second.binary_signaled = true;
                    it->second.highest_signal = std::max(it->second.highest_signal, signals[i].value);
                }
            }
        }

        struct memory_data
        {
            VkDeviceMemory handle{};
            uint64_t device_id{};
            uint64_t requested_size{};
            uint32_t memory_type_index{};
            uint32_t flags{};
            uint32_t device_mask{};
            void* persistent_host_pointer{};
            uint64_t mapped_size{};
            uint64_t allocation_size{};
            bool placed{};
            bool imported{};
            bool non_coherent{};
        };

        struct buffer_data
        {
            VkBuffer handle{};
            uint64_t device_id{};
            uint32_t usage{};
            uint64_t memory_id{};
            uint64_t memory_offset{};
            uint64_t size{};
        };

        struct image_create_params
        {
            uint32_t format{};
            uint32_t width{};
            uint32_t height{};
            uint32_t depth{};
            uint32_t mip_levels{};
            uint32_t array_layers{};
            uint32_t samples{};
            uint32_t usage{};
            uint32_t tiling{};
            uint32_t image_type{};
            uint32_t flags{};
        };

        struct image_data
        {
            VkImage handle{};
            uint64_t device_id{};
            uint32_t samples{1};
            bool owned{true};
            image_create_params create{};
            uint64_t memory_id{};
            uint64_t memory_offset{};
            // Whether the image was created with the transfer usage snapshots copy it with.
            bool copyable{};
            // The layout of each aspect, mip level and array layer as of the last submission, in the order of
            // subresource_index. Vulkan cannot report it.
            std::vector<uint32_t> layouts{};
            bool ownership_transferred{};
            VkQueue last_queue{};
            bool used_by_several_queues{};
            // A re-created swapchain image is undefined, while the guest still believes it is in its saved layout.
            bool layout_undefined{};

            uint32_t aspect_count() const
            {
                return static_cast<uint32_t>(std::popcount(get_vulkan_format_aspects(this->create.format)));
            }

            size_t subresource_index(const uint32_t aspect, const uint32_t mip, const uint32_t layer) const
            {
                return (static_cast<size_t>(aspect) * this->create.mip_levels + mip) * this->create.array_layers + layer;
            }
        };

        // What snapshots re-create objects from. What a later object needs only while it is created is shared with it, so
        // that it can still be re-created after the guest destroyed that dependency.
        struct shader_code
        {
            uint32_t flags{};
            std::vector<std::byte> code{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->flags);
                buffer.write_vector(this->code);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->flags);
                buffer.read_vector(this->code);
            }
        };

        struct set_layout_params
        {
            uint32_t flags{};
            std::vector<descriptor_binding> bindings{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->flags);
                buffer.write_vector(this->bindings);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->flags);
                buffer.read_vector(this->bindings);
            }
        };

        struct pipeline_layout_params
        {
            uint32_t push_constant_stages{};
            uint32_t push_constant_size{};
            std::vector<uint64_t> set_layouts{};
            std::vector<std::shared_ptr<const set_layout_params>> set_layout_creates{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->push_constant_stages);
                buffer.write(this->push_constant_size);
                buffer.write_vector(this->set_layouts);
                for (const auto& params : this->set_layout_creates)
                {
                    buffer.write(*params);
                }
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->push_constant_stages);
                buffer.read(this->push_constant_size);
                buffer.read_vector(this->set_layouts);
                this->set_layout_creates.clear();
                for (size_t i = 0; i < this->set_layouts.size(); ++i)
                {
                    this->set_layout_creates.push_back(std::make_shared<const set_layout_params>(buffer.read<set_layout_params>()));
                }
            }
        };

        struct render_pass_params
        {
            uint32_t format{};
            uint32_t load_op{};
            uint32_t store_op{};
            uint32_t initial_layout{};
            uint32_t final_layout{};
            uint32_t depth_format{};
        };

        struct shader_stage_params
        {
            uint64_t module{};
            std::vector<uint8_t> identifier{};
            // Missing for a stage given only by an identifier of code the bridge never saw.
            std::shared_ptr<const shader_code> code{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->module);
                buffer.write_vector(this->identifier);
                buffer.write(this->code != nullptr);
                if (this->code)
                {
                    buffer.write(*this->code);
                }
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->module);
                buffer.read_vector(this->identifier);
                this->code = buffer.read<bool>() ? std::make_shared<const shader_code>(buffer.read<shader_code>()) : nullptr;
            }
        };

        struct specialization_params
        {
            std::vector<spec_entry> entries{};
            std::vector<uint8_t> data{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write_vector(this->entries);
                buffer.write_vector(this->data);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read_vector(this->entries);
                buffer.read_vector(this->data);
            }
        };

        struct pipeline_params
        {
            bool compute{};
            uint64_t pipeline_cache{};
            uint64_t render_pass{};
            render_pass_params render_pass_create{};
            uint64_t pipeline_layout{};
            std::shared_ptr<const pipeline_layout_params> layout_create{};
            // The compute stage for compute pipelines.
            shader_stage_params vertex{};
            shader_stage_params fragment{};
            uint32_t flags{};
            uint32_t width{};
            uint32_t height{};
            std::vector<vertex_binding> bindings{};
            std::vector<vertex_attribute> attributes{};
            std::vector<vertex_divisor> divisors{};
            depth_state depth{};
            std::vector<uint32_t> color_formats{};
            uint32_t depth_format{};
            uint32_t stencil_format{};
            uint32_t rasterization_samples{};
            uint32_t primitive_topology{};
            uint32_t primitive_restart_enable{};
            uint32_t rasterization_stream{};
            uint32_t rasterization_stream_flags{};
            std::vector<uint32_t> dynamic_states{};
            specialization_params vs_spec{};
            specialization_params fs_spec{};
            std::vector<color_blend_attachment> blend_attachments{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->compute);
                buffer.write(this->pipeline_cache);
                buffer.write(this->render_pass);
                buffer.write(this->render_pass_create);
                buffer.write(this->pipeline_layout);
                buffer.write(*this->layout_create);
                buffer.write(this->vertex);
                buffer.write(this->fragment);
                buffer.write(this->flags);
                buffer.write(this->width);
                buffer.write(this->height);
                buffer.write_vector(this->bindings);
                buffer.write_vector(this->attributes);
                buffer.write_vector(this->divisors);
                buffer.write(this->depth);
                buffer.write_vector(this->color_formats);
                buffer.write(this->depth_format);
                buffer.write(this->stencil_format);
                buffer.write(this->rasterization_samples);
                buffer.write(this->primitive_topology);
                buffer.write(this->primitive_restart_enable);
                buffer.write(this->rasterization_stream);
                buffer.write(this->rasterization_stream_flags);
                buffer.write_vector(this->dynamic_states);
                buffer.write(this->vs_spec);
                buffer.write(this->fs_spec);
                buffer.write_vector(this->blend_attachments);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->compute);
                buffer.read(this->pipeline_cache);
                buffer.read(this->render_pass);
                buffer.read(this->render_pass_create);
                buffer.read(this->pipeline_layout);
                this->layout_create = std::make_shared<const pipeline_layout_params>(buffer.read<pipeline_layout_params>());
                buffer.read(this->vertex);
                buffer.read(this->fragment);
                buffer.read(this->flags);
                buffer.read(this->width);
                buffer.read(this->height);
                buffer.read_vector(this->bindings);
                buffer.read_vector(this->attributes);
                buffer.read_vector(this->divisors);
                buffer.read(this->depth);
                buffer.read_vector(this->color_formats);
                buffer.read(this->depth_format);
                buffer.read(this->stencil_format);
                buffer.read(this->rasterization_samples);
                buffer.read(this->primitive_topology);
                buffer.read(this->primitive_restart_enable);
                buffer.read(this->rasterization_stream);
                buffer.read(this->rasterization_stream_flags);
                buffer.read_vector(this->dynamic_states);
                buffer.read(this->vs_spec);
                buffer.read(this->fs_spec);
                buffer.read_vector(this->blend_attachments);
            }
        };

        struct image_view_params
        {
            uint64_t image{};
            uint32_t format{};
            uint32_t aspect_mask{};
            uint32_t view_type{};
            uint32_t base_mip_level{};
            uint32_t level_count{};
            uint32_t base_array_layer{};
            uint32_t layer_count{};
            std::array<uint32_t, 4> swizzle{};
        };

        struct buffer_view_params
        {
            uint64_t buffer{};
            uint32_t format{};
            uint64_t offset{};
            uint64_t range{};
        };

        struct framebuffer_params
        {
            uint64_t render_pass{};
            render_pass_params render_pass_create{};
            uint64_t image_view{};
            uint64_t depth_view{};
            uint32_t width{};
            uint32_t height{};
        };

        struct descriptor_pool_params
        {
            uint32_t max_sets{};
            uint32_t flags{};
            uint32_t max_inline_uniform_block_bindings{};
            std::vector<descriptor_pool_size> sizes{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->max_sets);
                buffer.write(this->flags);
                buffer.write(this->max_inline_uniform_block_bindings);
                buffer.write_vector(this->sizes);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->max_sets);
                buffer.read(this->flags);
                buffer.read(this->max_inline_uniform_block_bindings);
                buffer.read_vector(this->sizes);
            }
        };

        // The descriptor a set's binding and array element hold.
        struct descriptor_slot
        {
            uint32_t descriptor_type{};
            uint64_t buffer_or_view{};
            uint64_t offset{};
            uint64_t range{};
            uint64_t sampler{};
            uint64_t image_view{};
            uint32_t image_layout{};
        };

        static uint64_t descriptor_slot_key(const uint32_t binding, const uint32_t element)
        {
            return (static_cast<uint64_t>(binding) << 32) | element;
        }

        template <typename Set>
        static bool is_inline_block(const Set& set, const uint32_t binding)
        {
            return std::ranges::any_of(set.layout_create->bindings, [&](const descriptor_binding& entry) {
                return entry.binding == binding && entry.descriptor_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
            });
        }

        // The binding and array element of each descriptor an update of count descriptors starting at binding and element
        // reaches, which continues into the following bindings once one ends. A variable count applies to the last binding.
        template <typename Set>
        static std::vector<std::pair<uint32_t, uint32_t>> consecutive_descriptors(const Set& set, const uint32_t binding, uint32_t element,
                                                                                  uint32_t count)
        {
            auto bindings = set.layout_create->bindings;
            std::ranges::sort(bindings, {}, &descriptor_binding::binding);

            std::vector<std::pair<uint32_t, uint32_t>> descriptors{};
            auto it = std::ranges::find(bindings, binding, &descriptor_binding::binding);
            for (; it != bindings.end() && count > 0; ++it, element = 0)
            {
                const bool last = std::next(it) == bindings.end();
                const uint32_t size = last && set.variable_descriptor_count ? *set.variable_descriptor_count : it->descriptor_count;
                for (; element < size && count > 0; ++element, --count)
                {
                    descriptors.emplace_back(it->binding, element);
                }
            }

            return descriptors;
        }

        // Identifiers of shader code the guest asked for, so that pipelines it creates from them alone can be re-created.
        std::unordered_map<std::string, std::shared_ptr<const shader_code>> identified_code{};

        shader_stage_params stage_params(const shader_stage_source& source)
        {
            shader_stage_params params{.module = source.module, .identifier = {source.identifier.begin(), source.identifier.end()}};
            if (const auto module = this->shader_modules.find(source.module); module != this->shader_modules.end())
            {
                params.code = module->second.code;
            }
            else if (const auto code = this->identified_code.find(
                         std::string(reinterpret_cast<const char*>(source.identifier.data()), source.identifier.size()));
                     code != this->identified_code.end())
            {
                params.code = code->second;
            }
            return params;
        }

        struct sampler_data
        {
            VkSampler handle{};
            uint64_t device_id{};
            sampler_desc create{};
        };

        object_table<device_data> devices{&this->recording_references};
        object_table<queue_data> queues{&this->recording_references};
        // Vulkan requires host access to a queue to be externally synchronized. Several queue ids can name the same
        // native queue, so the locks are keyed by it.
        std::unordered_map<VkQueue, std::shared_ptr<std::mutex>> queue_locks;

        std::shared_ptr<std::mutex> queue_lock(const VkQueue queue)
        {
            auto& lock = this->queue_locks[queue];
            if (!lock)
            {
                lock = std::make_shared<std::mutex>();
            }
            return lock;
        }

        object_table<command_pool_data> command_pools{&this->recording_references};
        object_table<command_buffer_data> command_buffers{&this->recording_references};
        object_table<fence_data> fences{&this->recording_references};
        object_table<event_data> events{&this->recording_references};
        object_table<semaphore_data> semaphores{&this->recording_references};
        object_table<memory_data> memories{&this->recording_references};
        object_table<buffer_data> buffers{&this->recording_references};
        object_table<image_data> images{&this->recording_references};
        object_table<sampler_data> samplers{&this->recording_references};
        object_table<surface_data> surfaces{&this->recording_references};
        object_table<swapchain_data> swapchains{&this->recording_references};

        // Graphics-pipeline objects. All non-dispatchable, device-owned, so one shape suffices.
        struct shader_module_data
        {
            VkShaderModule handle{};
            uint64_t device_id{};
            std::shared_ptr<const shader_code> code{};
        };

        struct image_view_data
        {
            VkImageView handle{};
            uint64_t device_id{};
            image_view_params create{};
        };

        struct buffer_view_data
        {
            VkBufferView handle{};
            uint64_t device_id{};
            buffer_view_params create{};
        };

        struct query_pool_data
        {
            VkQueryPool handle{};
            uint64_t device_id{};
            uint32_t query_type{};
            uint32_t pipeline_statistics{};
            uint32_t query_count{};
            // Results that were available when the snapshot was taken, by query, until the guest resets the query. A
            // re-created pool cannot hold them.
            std::unordered_map<uint32_t, std::vector<uint64_t>> restored_results{};

            uint32_t value_count() const
            {
                return this->query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS
                           ? static_cast<uint32_t>(std::popcount(this->pipeline_statistics))
                           : 1;
            }

            void forget_restored(const uint32_t first_query, const uint32_t count)
            {
                if (this->restored_results.empty())
                {
                    return;
                }

                for (uint32_t query = first_query; query - first_query < count; ++query)
                {
                    this->restored_results.erase(query);
                }
            }

            // A result as vkGetQueryPoolResults and vkCmdCopyQueryPoolResults lay it out.
            std::vector<std::byte> restored_result(const uint32_t query, const uint32_t flags) const
            {
                const auto& values = this->restored_results.at(query);
                const bool wide = (flags & VK_QUERY_RESULT_64_BIT) != 0;
                std::vector<std::byte> result{};
                const auto append = [&](const uint64_t value) {
                    const auto narrow = static_cast<uint32_t>(value);
                    const auto* bytes = wide ? reinterpret_cast<const std::byte*>(&value) : reinterpret_cast<const std::byte*>(&narrow);
                    result.insert(result.end(), bytes, bytes + (wide ? sizeof(value) : sizeof(narrow)));
                };

                for (const auto value : values)
                {
                    append(value);
                }

                if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0)
                {
                    append(1);
                }

                return result;
            }
        };

        struct render_pass_data
        {
            VkRenderPass handle{};
            uint64_t device_id{};
            bool has_depth{};
            render_pass_params create{};
        };

        struct framebuffer_data
        {
            VkFramebuffer handle{};
            uint64_t device_id{};
            framebuffer_params create{};
        };

        struct pipeline_layout_data
        {
            VkPipelineLayout handle{};
            uint64_t device_id{};
            std::shared_ptr<const pipeline_layout_params> create{};
        };

        struct pipeline_data
        {
            VkPipeline handle{};
            uint64_t device_id{};
            pipeline_params create{};
        };

        struct pipeline_cache_data
        {
            VkPipelineCache handle{};
            uint64_t device_id{};
            uint32_t flags{};
        };

        struct descriptor_set_layout_data
        {
            VkDescriptorSetLayout handle{};
            uint64_t device_id{};
            std::shared_ptr<const set_layout_params> create{};
        };

        struct descriptor_pool_data
        {
            VkDescriptorPool handle{};
            uint64_t device_id{};
            descriptor_pool_params create{};
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
            uint64_t layout_id{};
            std::shared_ptr<const set_layout_params> layout_create{};
            std::optional<uint32_t> variable_descriptor_count{};
            std::unordered_map<uint64_t, descriptor_slot> slots{};
            // Inline uniform blocks by binding, written by byte offset.
            std::unordered_map<uint32_t, std::vector<std::byte>> inline_blocks{};
        };

        object_table<shader_module_data> shader_modules{&this->recording_references};
        object_table<image_view_data> image_views{&this->recording_references};
        object_table<buffer_view_data> buffer_views{&this->recording_references};
        object_table<query_pool_data> query_pools{&this->recording_references};
        object_table<render_pass_data> render_passes{&this->recording_references};
        object_table<framebuffer_data> framebuffers{&this->recording_references};
        object_table<pipeline_layout_data> pipeline_layouts{&this->recording_references};
        object_table<pipeline_data> pipelines{&this->recording_references};
        object_table<pipeline_cache_data> pipeline_caches{&this->recording_references};
        object_table<descriptor_set_layout_data> descriptor_set_layouts{&this->recording_references};
        object_table<descriptor_pool_data> descriptor_pools{&this->recording_references};
        object_table<descriptor_set_data> descriptor_sets{&this->recording_references};
        uint64_t next_id{1};

        // Set while restoring a snapshot, so the object being re-created gets the id the guest already holds.
        std::deque<uint64_t> forced_ids{};

        uint64_t allocate_id()
        {
            if (!this->forced_ids.empty())
            {
                const auto id = this->forced_ids.front();
                this->forced_ids.pop_front();
                return id;
            }

            return this->next_id++;
        }

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

        // Redirect a CPU-slow host-visible allocation to plain cached system RAM. DXVK places CPU-written
        // buffers (vertex/uniform/dynamic) in the fastest-for-the-GPU host-visible memory it can find, which
        // on modern GPUs is Resizable-BAR VRAM (DEVICE_LOCAL | HOST_VISIBLE) or write-combined system memory.
        // Optimal on real hardware, but the guest's memcpy into it goes through the hypervisor to uncached/WC
        // memory over PCIe, where reads run ~350x slower than cached RAM (~50 MB/s) and dominate frame time.
        // Substituting a HOST_CACHED system-RAM type makes guest access near-native; the GPU then reads those
        // buffers over PCIe instead of from local VRAM, which is what native drivers do for dynamic data
        // anyway. Only substitute a type that is also HOST_COHERENT: DXVK relies on coherency and never issues
        // vkFlushMappedMemoryRanges, so dropping it would leave the GPU reading stale data.
        //
        // Caveat: strictly, the substituted index must be set in the eventual resource's
        // VkMemoryRequirements::memoryTypeBits, but a bare vkAllocateMemory (this path) carries no such
        // bitmask and a VkDeviceMemory is not tied to one resource. We only ever swap one host-visible type
        // for another host-visible type, which desktop drivers permit for every host-visible buffer, so the
        // substituted type is compatible in practice. Set SOGEN_GPU_FORCE_CACHED=0 to disable.
        uint32_t substitute_cached_memory_type(const device_data& dev, const uint32_t index)
        {
            static const bool disabled = [] {
                const char* e = std::getenv("SOGEN_GPU_FORCE_CACHED");
                return e != nullptr && e[0] == '0';
            }();
            if (disabled)
            {
                return index;
            }

            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_physical_device_memory_properties)
            {
                return index;
            }

            VkPhysicalDeviceMemoryProperties props{};
            instance->second.get_physical_device_memory_properties(dev.physical_device, &props);
            if (index >= props.memoryTypeCount)
            {
                return index;
            }

            // Only touch host-visible memory that is not already cached (ReBAR VRAM or write-combined system
            // memory). Leave pure device-local VRAM and already-cached memory alone.
            const auto original = props.memoryTypes[index].propertyFlags;
            if (!(original & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (original & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
            {
                return index;
            }

            constexpr VkMemoryPropertyFlags want =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            {
                const auto flags = props.memoryTypes[i].propertyFlags;
                if ((flags & want) == want && !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                {
                    return i;
                }
            }
            return index;
        }

        // Fills `props` from the device's physical device. Returns false if the query is unavailable.
        bool query_memory_properties(const device_data& dev, VkPhysicalDeviceMemoryProperties& props)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_physical_device_memory_properties)
            {
                return false;
            }
            instance->second.get_physical_device_memory_properties(dev.physical_device, &props);
            return true;
        }

        std::unordered_map<uint64_t, std::vector<VkMappedMemoryRange>> to_mapped_ranges(
            const std::span<const vulkan_host::memory_range> ranges) const
        {
            std::unordered_map<uint64_t, std::vector<VkMappedMemoryRange>> device_ranges{};
            for (const auto& range : ranges)
            {
                const auto mem = this->memories.find(range.memory);
                if (mem == this->memories.end() || !mem->second.imported || !mem->second.non_coherent ||
                    range.offset >= mem->second.allocation_size)
                {
                    continue;
                }

                // A size that is not a multiple of nonCoherentAtomSize is only valid when it reaches the end of the memory.
                const bool reaches_end = range.size >= mem->second.allocation_size - range.offset;
                device_ranges[mem->second.device_id].push_back(VkMappedMemoryRange{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                                                                   .pNext = nullptr,
                                                                                   .memory = mem->second.handle,
                                                                                   .offset = range.offset,
                                                                                   .size = reaches_end ? VK_WHOLE_SIZE : range.size});
            }

            return device_ranges;
        }

        bool buffer_supports_host_import(const device_data& dev, const VkBufferCreateFlags flags, const VkBufferUsageFlags usage)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (dev.host_import_alignment == 0 || instance == this->instances.end() ||
                !instance->second.get_physical_device_external_buffer_properties)
            {
                return false;
            }

            const VkPhysicalDeviceExternalBufferInfo info{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                                                          .pNext = nullptr,
                                                          .flags = flags,
                                                          .usage = usage,
                                                          .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
            VkExternalBufferProperties properties{};
            properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
            instance->second.get_physical_device_external_buffer_properties(dev.physical_device, &info, &properties);
            return (properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
        }

        bool image_supports_host_import(const device_data& dev, const VkImageCreateInfo& image)
        {
            const auto instance = this->instances.find(dev.instance_id);
            if (dev.host_import_alignment == 0 || instance == this->instances.end() ||
                !instance->second.get_physical_device_image_format_properties2)
            {
                return false;
            }

            const VkPhysicalDeviceExternalImageFormatInfo external_info{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                .pNext = nullptr,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
            const VkPhysicalDeviceImageFormatInfo2 info{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                        .pNext = &external_info,
                                                        .format = image.format,
                                                        .type = image.imageType,
                                                        .tiling = image.tiling,
                                                        .usage = image.usage,
                                                        .flags = image.flags};
            VkExternalImageFormatProperties external_properties{};
            external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            properties.pNext = &external_properties;
            if (instance->second.get_physical_device_image_format_properties2(dev.physical_device, &info, &properties) != VK_SUCCESS ||
                (external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0)
            {
                return false;
            }

            // The limits can be tighter for external memory than for the image without it.
            const auto& limits = properties.imageFormatProperties;
            return image.extent.width <= limits.maxExtent.width && image.extent.height <= limits.maxExtent.height &&
                   image.extent.depth <= limits.maxExtent.depth && image.mipLevels <= limits.maxMipLevels &&
                   image.arrayLayers <= limits.maxArrayLayers && (limits.sampleCounts & image.samples) != 0;
        }

        template <typename Map, typename Pred>
        void erase_owned(Map& map, Pred pred)
        {
            for (auto it = map.begin(); it != map.end();)
            {
                it = pred(it->second) ? map.erase(it) : std::next(it);
            }
        }

        void destroy_swapchain_resources(swapchain_data& sc)
        {
            const auto dev_it = this->devices.find(sc.device_id);
            if (dev_it == this->devices.end())
            {
                return;
            }

            for (const uint64_t image_id : sc.image_ids)
            {
                this->images.erase(image_id);
            }
            sc.image_ids.clear();

            if (sc.handle && dev_it->second.destroy_swapchain)
            {
                dev_it->second.destroy_swapchain(dev_it->second.handle, sc.handle, nullptr);
            }
            sc.handle = VK_NULL_HANDLE;
        }

        void erase_device(uint64_t device_id)
        {
            const auto it = this->devices.find(device_id);
            if (it == this->devices.end())
            {
                return;
            }

            const auto presents = it->second.presents;
            const std::unique_lock presents_done(*presents);

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
            for (auto& [id, cache] : this->pipeline_caches)
            {
                if (cache.device_id == device_id && cache.handle && it->second.destroy_pipeline_cache)
                {
                    it->second.destroy_pipeline_cache(it->second.handle, cache.handle, nullptr);
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
            this->erase_owned(this->pipeline_caches, [&](const pipeline_cache_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->pipeline_layouts, [&](const pipeline_layout_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->render_passes, [&](const render_pass_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->image_views, [&](const image_view_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->buffer_views, [&](const buffer_view_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->query_pools, [&](const query_pool_data& d) { return d.device_id == device_id; });
            this->erase_owned(this->shader_modules, [&](const shader_module_data& d) { return d.device_id == device_id; });

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
            // A fence still pending here belongs to a queue that never finished: no teardown of it is valid, so it is left to
            // the device's destruction.
            for (const auto fence : it->second.abandoned_drain_fences)
            {
                if (it->second.get_fence_status(it->second.handle, fence) == VK_SUCCESS)
                {
                    it->second.destroy_fence(it->second.handle, fence, nullptr);
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
                if (image.device_id == device_id && image.owned && image.handle && it->second.destroy_image)
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
            for (const auto& queue : this->queues | std::views::values)
            {
                if (queue.device_id == device_id)
                {
                    this->queue_locks.erase(queue.handle);
                }
            }
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
            while (!this->devices.empty())
            {
                this->erase_device(this->devices.begin()->first);
            }

            for (const auto& [id, surface] : this->surfaces)
            {
                const auto instance = this->instances.find(surface.instance_id);
                if (instance != this->instances.end() && surface.handle && instance->second.destroy_surface)
                {
                    instance->second.destroy_surface(instance->second.handle, surface.handle, nullptr);
                }
            }
            this->surfaces.clear();

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

    int32_t vulkan_host::create_instance(std::span<const std::string> extensions, uint64_t& out_instance)
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

        std::vector<const char*> extension_names;
        extension_names.reserve(extensions.size());
        VkInstanceCreateFlags instance_flags = 0;
        for (const auto& extension : extensions)
        {
            if (extension.empty())
            {
                continue;
            }
            extension_names.push_back(extension.c_str());
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
            if (extension == VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
            {
                instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
#endif
        }

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.flags = instance_flags;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
        create_info.ppEnabledExtensionNames = extension_names.empty() ? nullptr : extension_names.data();

        // With a portability driver installed (MoltenVK on macOS) the loader fails vkCreateInstance with
        // VK_ERROR_INCOMPATIBLE_DRIVER unless the caller enables VK_KHR_portability_enumeration and sets
        // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR.
        if (const auto enumerate_instance_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                this->impl_->get_instance_proc_addr(nullptr, "vkEnumerateInstanceExtensionProperties")))
        {
            uint32_t ext_count = 0;
            std::vector<VkExtensionProperties> available;
            VkResult enumerate_result = enumerate_instance_extensions(nullptr, &ext_count, nullptr);
            if ((enumerate_result == VK_SUCCESS || enumerate_result == VK_INCOMPLETE) && ext_count > 0)
            {
                available.resize(ext_count);
                enumerate_result = enumerate_instance_extensions(nullptr, &ext_count, available.data());
                if (enumerate_result != VK_SUCCESS && enumerate_result != VK_INCOMPLETE)
                {
                    available.clear();
                }
            }

            const bool has_portability_enumeration = std::ranges::any_of(available, [](const VkExtensionProperties& ext) {
                return std::string_view{static_cast<const char*>(ext.extensionName)} == VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            });
            if (has_portability_enumeration && (create_info.flags & VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR) == 0)
            {
                extension_names.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
        }
        create_info.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
        create_info.ppEnabledExtensionNames = extension_names.empty() ? nullptr : extension_names.data();

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
        data.get_queue_family_properties2 = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
        data.get_physical_device_memory_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties");
        data.get_physical_device_memory_properties2 =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceMemoryProperties2>(instance, "vkGetPhysicalDeviceMemoryProperties2");
        data.get_physical_device_features2 =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
        data.get_physical_device_properties2 =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
        data.get_physical_device_image_format_properties2 = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            instance, "vkGetPhysicalDeviceImageFormatProperties2");
        data.get_physical_device_external_buffer_properties =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceExternalBufferProperties>(instance,
                                                                                             "vkGetPhysicalDeviceExternalBufferProperties");
        data.enumerate_device_extension_properties =
            this->impl_->load_instance_proc<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
        data.destroy_surface = this->impl_->load_instance_proc<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR");
        data.get_surface_support =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
        data.get_surface_capabilities = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        data.get_surface_formats =
            this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        data.get_surface_present_modes = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        data.get_cooperative_matrix_properties = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
            instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
        data.get_fragment_shading_rates = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
            instance, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
        data.get_calibrateable_time_domains = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
            instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
        if (!data.get_calibrateable_time_domains)
        {
            data.get_calibrateable_time_domains = this->impl_->load_instance_proc<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        }
        data.create_device = this->impl_->load_instance_proc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
        data.get_device_proc_addr = this->impl_->load_instance_proc<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        data.extensions.assign(extensions.begin(), extensions.end());

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->instances.emplace(id, data);
        out_instance = id;
        return VK_SUCCESS;
    }

    bool vulkan_host::get_native_instance_handle(uint64_t instance, uint64_t& out_handle) const
    {
        out_handle = 0;
        const auto it = this->impl_->instances.find(instance);
        if (it == this->impl_->instances.end() || !it->second.handle)
        {
            return false;
        }
        out_handle = reinterpret_cast<uint64_t>(it->second.handle);
        return true;
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

        for (auto surface = this->impl_->surfaces.begin(); surface != this->impl_->surfaces.end();)
        {
            if (surface->second.instance_id == instance)
            {
                if (surface->second.handle && it->second.destroy_surface)
                {
                    it->second.destroy_surface(it->second.handle, surface->second.handle, nullptr);
                }
                surface = this->impl_->surfaces.erase(surface);
            }
            else
            {
                ++surface;
            }
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
                const uint64_t id = this->impl_->allocate_id();
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

    int32_t vulkan_host::get_queue_family_properties(uint64_t physical_device, const bool query_ownership_transfer, void* out,
                                                     size_t out_size, uint32_t& out_count)
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

        std::vector<gpu_bridge::queue_family_properties> wire_families(count);
        if (query_ownership_transfer && instance->second.get_queue_family_properties2)
        {
            std::vector<VkQueueFamilyProperties2> families(count);
            std::vector<VkQueueFamilyOwnershipTransferPropertiesKHR> ownership(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
                families[i].pNext = &ownership[i];
                ownership[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR;
            }
            instance->second.get_queue_family_properties2(pd->second.handle, &count, families.data());
            wire_families.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& family = families[i].queueFamilyProperties;
                wire_families[i] = {.queue_flags = family.queueFlags,
                                    .queue_count = family.queueCount,
                                    .timestamp_valid_bits = family.timestampValidBits,
                                    .min_image_transfer_granularity_width = family.minImageTransferGranularity.width,
                                    .min_image_transfer_granularity_height = family.minImageTransferGranularity.height,
                                    .min_image_transfer_granularity_depth = family.minImageTransferGranularity.depth,
                                    .optimal_image_transfer_to_queue_families = ownership[i].optimalImageTransferToQueueFamilies};
            }
        }
        else
        {
            std::vector<VkQueueFamilyProperties> families(count);
            if (count > 0)
            {
                instance->second.get_queue_family_properties(pd->second.handle, &count, families.data());
            }
            wire_families.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& family = families[i];
                wire_families[i] = {.queue_flags = family.queueFlags,
                                    .queue_count = family.queueCount,
                                    .timestamp_valid_bits = family.timestampValidBits,
                                    .min_image_transfer_granularity_width = family.minImageTransferGranularity.width,
                                    .min_image_transfer_granularity_height = family.minImageTransferGranularity.height,
                                    .min_image_transfer_granularity_depth = family.minImageTransferGranularity.depth,
                                    .optimal_image_transfer_to_queue_families = 0};
            }
        }

        out_count = count;

        const size_t copy_bytes = std::min(out_size, wire_families.size() * sizeof(gpu_bridge::queue_family_properties));
        if (copy_bytes > 0)
        {
            std::memcpy(out, wire_families.data(), copy_bytes);
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

            extensions.resize(count);
        }

        const auto removed = std::ranges::remove_if(extensions, is_unsupported_device_extension);
        extensions.erase(removed.begin(), removed.end());

        // DXVK's adapter filter requires VK_EXT_depth_clip_enable, which MoltenVK lacks; advertised here
        // and stripped again in create_device (limitations: docs/gpu-paravirtualization.md).
        if (impl::is_portability_device(instance->second, pd->second))
        {
            const bool has_depth_clip = std::ranges::any_of(extensions, [](const VkExtensionProperties& ext) {
                return std::strcmp(ext.extensionName, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) == 0;
            });
            if (!has_depth_clip)
            {
                VkExtensionProperties synthetic{};
                std::strncpy(synthetic.extensionName, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, sizeof(synthetic.extensionName) - 1);
                synthetic.specVersion = VK_EXT_DEPTH_CLIP_ENABLE_SPEC_VERSION;
                extensions.push_back(synthetic);
            }
        }

        out_count = static_cast<uint32_t>(extensions.size());

        const size_t copy_bytes = std::min(out_size, extensions.size() * sizeof(VkExtensionProperties));
        if (copy_bytes > 0)
        {
            std::memcpy(out, extensions.data(), copy_bytes);
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_physical_device_cooperative_matrix_properties(uint64_t physical_device, void* out, size_t out_size,
                                                                           uint32_t max_count, bool has_entries, uint32_t& out_count)
    {
        out_count = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!instance->second.get_cooperative_matrix_properties)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        uint32_t count = max_count;
        if (!has_entries)
        {
            const VkResult result = instance->second.get_cooperative_matrix_properties(pd->second.handle, &count, nullptr);
            out_count = count;
            return result;
        }

        std::vector<VkCooperativeMatrixPropertiesKHR> properties(std::max(max_count, 1u));
        for (auto& property : properties)
        {
            property.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
            property.pNext = nullptr;
        }

        const VkResult result = instance->second.get_cooperative_matrix_properties(pd->second.handle, &count, properties.data());
        out_count = count;

        const size_t capacity = out_size / sizeof(gpu_bridge::cooperative_matrix_property);
        const size_t written = std::min<size_t>(count, std::min<size_t>(max_count, capacity));
        auto* wire = static_cast<gpu_bridge::cooperative_matrix_property*>(out);
        for (size_t i = 0; i < written; ++i)
        {
            wire[i] = {
                .m_size = properties[i].MSize,
                .n_size = properties[i].NSize,
                .k_size = properties[i].KSize,
                .a_type = static_cast<uint32_t>(properties[i].AType),
                .b_type = static_cast<uint32_t>(properties[i].BType),
                .c_type = static_cast<uint32_t>(properties[i].CType),
                .result_type = static_cast<uint32_t>(properties[i].ResultType),
                .saturating_accumulation = properties[i].saturatingAccumulation,
                .scope = static_cast<uint32_t>(properties[i].scope),
            };
        }
        return result;
    }

    int32_t vulkan_host::get_physical_device_fragment_shading_rates(uint64_t physical_device, void* out, size_t out_size,
                                                                    uint32_t max_count, bool has_entries, uint32_t& out_count)
    {
        out_count = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!instance->second.get_fragment_shading_rates)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        uint32_t count = max_count;
        if (!has_entries)
        {
            const VkResult result = instance->second.get_fragment_shading_rates(pd->second.handle, &count, nullptr);
            out_count = count;
            return result;
        }

        std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(std::max(max_count, 1u));
        for (auto& rate : rates)
        {
            rate.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
            rate.pNext = nullptr;
        }

        const VkResult result = instance->second.get_fragment_shading_rates(pd->second.handle, &count, rates.data());
        out_count = count;

        const size_t capacity = out_size / sizeof(gpu_bridge::fragment_shading_rate);
        const size_t written = std::min<size_t>(count, std::min<size_t>(max_count, capacity));
        auto* wire = static_cast<gpu_bridge::fragment_shading_rate*>(out);
        for (size_t i = 0; i < written; ++i)
        {
            wire[i] = {
                .sample_counts = rates[i].sampleCounts,
                .width = rates[i].fragmentSize.width,
                .height = rates[i].fragmentSize.height,
            };
        }
        return result;
    }

    int32_t vulkan_host::get_physical_device_calibrateable_time_domains(uint64_t physical_device, std::span<uint32_t> out_domains,
                                                                        uint32_t max_count, bool has_entries, uint32_t& out_count)
    {
        out_count = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!instance->second.get_calibrateable_time_domains)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        uint32_t count = max_count;
        if (!has_entries)
        {
            const VkResult result = instance->second.get_calibrateable_time_domains(pd->second.handle, &count, nullptr);
            out_count = count;
            return result;
        }

        std::vector<VkTimeDomainKHR> domains(std::max(max_count, 1u));
        const VkResult result = instance->second.get_calibrateable_time_domains(pd->second.handle, &count, domains.data());
        out_count = count;
        const size_t written = std::min<size_t>(count, std::min<size_t>(max_count, out_domains.size()));
        for (size_t i = 0; i < written; ++i)
        {
            out_domains[i] = static_cast<uint32_t>(domains[i]);
        }
        return result;
    }

    int32_t vulkan_host::get_calibrated_timestamps(uint64_t device, std::span<const uint32_t> time_domains, std::span<uint64_t> timestamps,
                                                   uint64_t& max_deviation)
    {
        max_deviation = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || time_domains.size() != timestamps.size())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!dev->second.get_calibrated_timestamps)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        std::vector<VkCalibratedTimestampInfoKHR> timestamp_infos(time_domains.size());
        for (size_t i = 0; i < time_domains.size(); ++i)
        {
            timestamp_infos[i].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
            timestamp_infos[i].timeDomain = static_cast<VkTimeDomainKHR>(time_domains[i]);
        }
        return dev->second.get_calibrated_timestamps(dev->second.handle, static_cast<uint32_t>(timestamp_infos.size()),
                                                     timestamp_infos.data(), timestamps.data(), &max_deviation);
    }

    int32_t vulkan_host::get_physical_device_features2(uint64_t physical_device, const void* in_records, size_t in_records_size,
                                                       uint32_t struct_count, const bool spoof_missing_features,
                                                       std::vector<std::byte>& out_blob)
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

        for (auto& buffer : chained)
        {
            auto* base = reinterpret_cast<VkBaseOutStructure*>(buffer.data());
            if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)
            {
                auto* features = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(buffer.data());
                // TODO: Expose geometry-stream support after geometry shaders are bridged.
                features->geometryStreams = VK_FALSE;
            }
        }

        // DXVK refuses adapters without these, but Mali GPUs lack them and older Adreno 6xx GPUs lack the 16- and
        // 8-bit storage access even with Turnip. Most games never touch them, and BC formats still report as
        // unsupported, so games fall back instead of failing. create_device masks them back out.
        if (spoof_missing_features)
        {
            features2.features.fillModeNonSolid = VK_TRUE;
            features2.features.multiViewport = VK_TRUE;
            features2.features.shaderClipDistance = VK_TRUE;
            features2.features.shaderCullDistance = VK_TRUE;
            features2.features.textureCompressionBC = VK_TRUE;

            for (auto& buffer : chained)
            {
                switch (reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType)
                {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                    reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(buffer.data())->storageBuffer16BitAccess = VK_TRUE;
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
                    reinterpret_cast<VkPhysicalDevice16BitStorageFeatures*>(buffer.data())->storageBuffer16BitAccess = VK_TRUE;
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                    reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(buffer.data())->storageBuffer8BitAccess = VK_TRUE;
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
                    reinterpret_cast<VkPhysicalDevice8BitStorageFeatures*>(buffer.data())->storageBuffer8BitAccess = VK_TRUE;
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
                    // Mali has the extension but not the stricter bounds checking; core robustBufferAccess still applies.
                    reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(buffer.data())->robustBufferAccess2 = VK_TRUE;
                    break;
                default:
                    break;
                }
            }
        }

        // DXVK's shared D3D8-11 adapter filter requires these five features, which MoltenVK lacks and
        // D3D9 never uses; create_device masks them back out before they reach the driver.
        if (impl::is_portability_device(instance->second, pd->second))
        {
            features2.features.geometryShader = VK_TRUE;
            features2.features.shaderCullDistance = VK_TRUE;

            for (auto& buffer : chained)
            {
                switch (reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType)
                {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
                    reinterpret_cast<VkPhysicalDeviceDepthClipEnableFeaturesEXT*>(buffer.data())->depthClipEnable = VK_TRUE;
                    break;
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
                    auto* robustness2 = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(buffer.data());
                    robustness2->robustBufferAccess2 = VK_TRUE;
                    robustness2->nullDescriptor = VK_TRUE;
                    break;
                }
                default:
                    break;
                }
            }
        }

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

        for (auto& buffer : chained)
        {
            auto* base = reinterpret_cast<VkBaseOutStructure*>(buffer.data());
            if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT)
            {
                auto* properties = reinterpret_cast<VkPhysicalDeviceTransformFeedbackPropertiesEXT*>(buffer.data());
                // TODO: Expose multiple transform-feedback streams after geometry shaders are bridged.
                properties->maxTransformFeedbackStreams = std::min(properties->maxTransformFeedbackStreams, 1u);
                properties->transformFeedbackStreamsLinesTriangles = VK_FALSE;
                properties->transformFeedbackRasterizationStreamSelect = VK_FALSE;
            }
        }

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
                                       const bool spoof_missing_features, uint64_t& out_device)
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
        // A real queue family exposes only a handful of queues; reject an absurd guest-supplied
        // queue_count before it sizes the priorities allocation below.
        constexpr uint32_t max_queues_per_family = 64;
        for (size_t i = 0; i < entry_count; ++i)
        {
            const uint32_t family = entries ? entries[i].queue_family_index : 0;
            const uint32_t requested = entries ? entries[i].queue_count : 1;
            if (requested > max_queues_per_family)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
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

        // Rebuild the enabled-extension name list from the blob (count NUL-terminated strings). Strip the same
        // extensions enumeration hides from the guest -- a guest is untrusted and may enable one we never
        // advertised, and we cannot marshal them across the bridge.
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
                if (!is_unsupported_extension_name(std::string_view{cursor}))
                {
                    extensions.push_back(cursor);
                }
                cursor = terminator + 1;
            }
        }

        const bool portability = impl::is_portability_device(instance->second, pd->second);

        // Vulkan requires VK_KHR_portability_subset to be enabled whenever the device advertises it, and
        // the guest never asks for it.
        const bool requests_portability_subset = std::ranges::any_of(
            extensions, [](const char* name) { return std::string_view{name} == impl::portability_subset_extension_name; });
        if (portability && !requests_portability_subset)
        {
            extensions.push_back(impl::portability_subset_extension_name.data());
        }

        // vkCreateDevice rejects an enabled extension the driver does not implement, so undo the
        // enumerate-time VK_EXT_depth_clip_enable spoof.
        if (portability && !impl::has_device_extension(instance->second, pd->second.handle, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME))
        {
            std::erase_if(extensions, [](const char* name) { return std::strcmp(name, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) == 0; });
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
                        if (type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)
                        {
                            auto* transform_feedback = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(buffer.data());
                            // TODO: Enable geometryStreams after geometry shaders are bridged.
                            transform_feedback->geometryStreams = VK_FALSE;
                        }
                        feature_tail->pNext = base;
                        feature_tail = base;
                    }
                }
                cursor += record.body_size;
            }
        }

        const bool has_feature_chain = has_features || !chained.empty();

        // Undo the query-time feature spoofs: vkCreateDevice fails on a feature the device lacks.
        if ((portability || spoof_missing_features) && has_feature_chain && instance->second.get_physical_device_features2)
        {
            VkPhysicalDeviceFeatures2 supported{};
            supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            std::vector<std::vector<std::byte>> supported_chained;
            supported_chained.reserve(chained.size());
            auto* supported_tail = reinterpret_cast<VkBaseOutStructure*>(&supported);
            for (const auto& buffer : chained)
            {
                auto& mirror = supported_chained.emplace_back(buffer.size(), std::byte{});
                auto* base = reinterpret_cast<VkBaseOutStructure*>(mirror.data());
                base->sType = reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType;
                base->pNext = nullptr;
                supported_tail->pNext = base;
                supported_tail = base;
            }
            instance->second.get_physical_device_features2(pd->second.handle, &supported);

            auto* enabled = reinterpret_cast<VkBool32*>(&features2.features);
            const auto* real = reinterpret_cast<const VkBool32*>(&supported.features);
            for (size_t i = 0; i < sizeof(features2.features) / sizeof(VkBool32); ++i)
            {
                enabled[i] &= real[i];
            }
            for (size_t c = 0; c < chained.size(); ++c)
            {
                const size_t body_bytes = chained[c].size() - gpu_bridge::feature_chain_header_size;
                auto* enabled_body = reinterpret_cast<VkBool32*>(chained[c].data() + gpu_bridge::feature_chain_header_size);
                const auto* real_body =
                    reinterpret_cast<const VkBool32*>(supported_chained[c].data() + gpu_bridge::feature_chain_header_size);
                for (size_t i = 0; i < body_bytes / sizeof(VkBool32); ++i)
                {
                    enabled_body[i] &= real_body[i];
                }
            }
        }

        // Placed mappings let the bridge map memory exactly where a backend with fixed guest addresses needs it (see
        // map_memory_placed). They are enabled on the host device only; the guest does not depend on them.
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed_features{};
        placed_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        uint64_t placed_map_alignment = 0;
        const bool guest_chains_placed_features = std::ranges::any_of(chained, [](const std::vector<std::byte>& buffer) {
            return reinterpret_cast<const VkBaseOutStructure*>(buffer.data())->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        });
        if (!guest_chains_placed_features && instance->second.get_physical_device_features2 &&
            instance->second.get_physical_device_properties2 &&
            impl::has_device_extension(instance->second, pd->second.handle, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) &&
            impl::has_device_extension(instance->second, pd->second.handle, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME))
        {
            VkPhysicalDeviceFeatures2 supported{};
            supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            supported.pNext = &placed_features;
            instance->second.get_physical_device_features2(pd->second.handle, &supported);

            if (placed_features.memoryMapPlaced && placed_features.memoryUnmapReserve)
            {
                VkPhysicalDeviceMapMemoryPlacedPropertiesEXT placed_properties{};
                placed_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT;
                VkPhysicalDeviceProperties2 properties{};
                properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties.pNext = &placed_properties;
                instance->second.get_physical_device_properties2(pd->second.handle, &properties);
                placed_map_alignment = placed_properties.minPlacedMemoryMapAlignment;

                placed_features.pNext = nullptr;
                placed_features.memoryMapRangePlaced = VK_FALSE;
                feature_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&placed_features);
                feature_tail = reinterpret_cast<VkBaseOutStructure*>(&placed_features);

                for (const char* name : {VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME})
                {
                    if (std::ranges::none_of(extensions, [&](const char* enabled) { return std::strcmp(enabled, name) == 0; }))
                    {
                        extensions.push_back(name);
                    }
                }
            }
        }

        // Without placed mappings, memory the guest maps can still be put at a fixed address by importing host memory
        // that already sits there.
        uint64_t host_import_alignment = 0;
        if (placed_map_alignment == 0 && instance->second.get_physical_device_properties2 &&
            impl::has_device_extension(instance->second, pd->second.handle, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
        {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties{};
            host_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &host_properties;
            instance->second.get_physical_device_properties2(pd->second.handle, &properties);
            host_import_alignment = host_properties.minImportedHostPointerAlignment;

            if (host_import_alignment != 0 && std::ranges::none_of(extensions, [](const char* enabled) {
                    return std::strcmp(enabled, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
                }))
            {
                extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
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
        if (has_feature_chain || placed_map_alignment != 0)
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
        data.create.physical_device = physical_device;
        if (entries)
        {
            data.create.queues.assign(entries, entries + queue_entry_count);
        }
        data.create.extensions.assign(static_cast<const std::byte*>(extension_blob),
                                      static_cast<const std::byte*>(extension_blob) + extension_blob_size);
        data.create.extension_count = extension_count;
        data.create.features.assign(static_cast<const std::byte*>(feature_blob),
                                    static_cast<const std::byte*>(feature_blob) + feature_blob_size);
        data.create.feature_count = feature_struct_count;
        data.create.spoof_missing_features = spoof_missing_features;
        const bool maintenance5_extension_enabled =
            std::ranges::any_of(extensions, [](const char* name) { return std::strcmp(name, "VK_KHR_maintenance5") == 0; });
        const bool maintenance5_feature_enabled = std::ranges::any_of(chained, [](const std::vector<std::byte>& buffer) {
            const auto* base = reinterpret_cast<const VkBaseInStructure*>(buffer.data());
            return base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR &&
                   reinterpret_cast<const VkPhysicalDeviceMaintenance5FeaturesKHR*>(base)->maintenance5 == VK_TRUE;
        });
        data.maintenance5_enabled = maintenance5_extension_enabled && maintenance5_feature_enabled;
        data.instance_id = pd->second.instance_id;
        data.physical_device = pd->second.handle;
        data.queue_family_index = primary_family;

        if (spoof_missing_features && instance->second.get_physical_device_features2)
        {
            VkPhysicalDeviceFeatures2 supported{};
            supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            instance->second.get_physical_device_features2(pd->second.handle, &supported);
            data.strip_clip_distance = supported.features.shaderClipDistance == VK_FALSE;
            data.strip_cull_distance = supported.features.shaderCullDistance == VK_FALSE;
        }

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
            data.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolve("vkCreateSwapchainKHR"));
            data.destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(resolve("vkDestroySwapchainKHR"));
            data.get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve("vkGetSwapchainImagesKHR"));
            data.acquire_next_image = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve("vkAcquireNextImageKHR"));
            data.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(resolve("vkQueuePresentKHR"));
            data.allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(resolve("vkAllocateMemory"));
            data.free_memory = reinterpret_cast<PFN_vkFreeMemory>(resolve("vkFreeMemory"));
            data.get_device_memory_commitment = reinterpret_cast<PFN_vkGetDeviceMemoryCommitment>(resolve("vkGetDeviceMemoryCommitment"));
            data.get_calibrated_timestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(resolve("vkGetCalibratedTimestampsKHR"));
            if (!data.get_calibrated_timestamps)
            {
                data.get_calibrated_timestamps =
                    reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(resolve("vkGetCalibratedTimestampsEXT"));
            }
            data.map_memory = reinterpret_cast<PFN_vkMapMemory>(resolve("vkMapMemory"));
            data.unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(resolve("vkUnmapMemory"));
            if (placed_map_alignment != 0)
            {
                data.map_memory2 = reinterpret_cast<PFN_vkMapMemory2KHR>(resolve("vkMapMemory2KHR"));
                data.unmap_memory2 = reinterpret_cast<PFN_vkUnmapMemory2KHR>(resolve("vkUnmapMemory2KHR"));
                data.placed_map_alignment = data.map_memory2 && data.unmap_memory2 ? placed_map_alignment : 0;
            }
            if (host_import_alignment != 0)
            {
                data.get_memory_host_pointer_properties =
                    reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(resolve("vkGetMemoryHostPointerPropertiesEXT"));
                data.host_import_alignment = data.get_memory_host_pointer_properties ? host_import_alignment : 0;
            }
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
            data.cmd_clear_attachments = reinterpret_cast<PFN_vkCmdClearAttachments>(resolve("vkCmdClearAttachments"));
            data.cmd_clear_depth_stencil_image = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(resolve("vkCmdClearDepthStencilImage"));
            data.cmd_copy_image_to_buffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(resolve("vkCmdCopyImageToBuffer"));
            data.cmd_resolve_image = reinterpret_cast<PFN_vkCmdResolveImage>(resolve("vkCmdResolveImage"));
            data.cmd_update_buffer = reinterpret_cast<PFN_vkCmdUpdateBuffer>(resolve("vkCmdUpdateBuffer"));
            data.cmd_copy_buffer_to_image = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(resolve("vkCmdCopyBufferToImage"));
            data.cmd_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(resolve("vkCmdCopyImage"));
            data.cmd_blit_image = reinterpret_cast<PFN_vkCmdBlitImage>(resolve("vkCmdBlitImage"));
            data.cmd_copy_buffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(resolve("vkCmdCopyBuffer"));
            data.create_sampler = reinterpret_cast<PFN_vkCreateSampler>(resolve("vkCreateSampler"));
            data.destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(resolve("vkDestroySampler"));
            data.create_shader_module = reinterpret_cast<PFN_vkCreateShaderModule>(resolve("vkCreateShaderModule"));
            data.destroy_shader_module = reinterpret_cast<PFN_vkDestroyShaderModule>(resolve("vkDestroyShaderModule"));
            data.get_shader_module_identifier =
                reinterpret_cast<PFN_vkGetShaderModuleIdentifierEXT>(resolve("vkGetShaderModuleIdentifierEXT"));
            data.get_shader_module_create_info_identifier =
                reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(resolve("vkGetShaderModuleCreateInfoIdentifierEXT"));
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
            data.cmd_begin_query_indexed = reinterpret_cast<PFN_vkCmdBeginQueryIndexedEXT>(resolve("vkCmdBeginQueryIndexedEXT"));
            data.cmd_end_query_indexed = reinterpret_cast<PFN_vkCmdEndQueryIndexedEXT>(resolve("vkCmdEndQueryIndexedEXT"));
            data.cmd_bind_transform_feedback_buffers =
                reinterpret_cast<PFN_vkCmdBindTransformFeedbackBuffersEXT>(resolve("vkCmdBindTransformFeedbackBuffersEXT"));
            data.cmd_begin_transform_feedback =
                reinterpret_cast<PFN_vkCmdBeginTransformFeedbackEXT>(resolve("vkCmdBeginTransformFeedbackEXT"));
            data.cmd_end_transform_feedback = reinterpret_cast<PFN_vkCmdEndTransformFeedbackEXT>(resolve("vkCmdEndTransformFeedbackEXT"));
            data.cmd_draw_indirect_byte_count =
                reinterpret_cast<PFN_vkCmdDrawIndirectByteCountEXT>(resolve("vkCmdDrawIndirectByteCountEXT"));
            data.cmd_write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(resolve("vkCmdWriteTimestamp"));
            data.cmd_write_timestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2>(resolve("vkCmdWriteTimestamp2"));
            if (!data.cmd_write_timestamp2)
            {
                data.cmd_write_timestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2>(resolve("vkCmdWriteTimestamp2KHR"));
            }
            data.cmd_copy_query_pool_results = reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(resolve("vkCmdCopyQueryPoolResults"));
            data.create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(resolve("vkCreateRenderPass"));
            data.destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(resolve("vkDestroyRenderPass"));
            data.create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(resolve("vkCreateFramebuffer"));
            data.destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(resolve("vkDestroyFramebuffer"));
            data.create_pipeline_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(resolve("vkCreatePipelineLayout"));
            data.destroy_pipeline_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(resolve("vkDestroyPipelineLayout"));
            data.create_descriptor_set_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(resolve("vkCreateDescriptorSetLayout"));
            data.get_descriptor_set_layout_support =
                reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(resolve("vkGetDescriptorSetLayoutSupport"));
            if (!data.get_descriptor_set_layout_support)
            {
                data.get_descriptor_set_layout_support =
                    reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(resolve("vkGetDescriptorSetLayoutSupportKHR"));
            }
            data.destroy_descriptor_set_layout =
                reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(resolve("vkDestroyDescriptorSetLayout"));
            data.create_descriptor_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(resolve("vkCreateDescriptorPool"));
            data.destroy_descriptor_pool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(resolve("vkDestroyDescriptorPool"));
            data.reset_descriptor_pool = reinterpret_cast<PFN_vkResetDescriptorPool>(resolve("vkResetDescriptorPool"));
            data.allocate_descriptor_sets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(resolve("vkAllocateDescriptorSets"));
            data.free_descriptor_sets = reinterpret_cast<PFN_vkFreeDescriptorSets>(resolve("vkFreeDescriptorSets"));
            data.update_descriptor_sets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(resolve("vkUpdateDescriptorSets"));
            data.cmd_bind_descriptor_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(resolve("vkCmdBindDescriptorSets"));
            data.create_pipeline_cache = reinterpret_cast<PFN_vkCreatePipelineCache>(resolve("vkCreatePipelineCache"));
            data.destroy_pipeline_cache = reinterpret_cast<PFN_vkDestroyPipelineCache>(resolve("vkDestroyPipelineCache"));
            data.get_pipeline_cache_data = reinterpret_cast<PFN_vkGetPipelineCacheData>(resolve("vkGetPipelineCacheData"));
            data.merge_pipeline_caches = reinterpret_cast<PFN_vkMergePipelineCaches>(resolve("vkMergePipelineCaches"));
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
            if (data.maintenance5_enabled)
            {
                data.cmd_bind_index_buffer2 = reinterpret_cast<PFN_vkCmdBindIndexBuffer2KHR>(resolve("vkCmdBindIndexBuffer2KHR"));
            }
            data.cmd_draw_indexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(resolve("vkCmdDrawIndexed"));
            data.cmd_draw_indexed_indirect = reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(resolve("vkCmdDrawIndexedIndirect"));
            data.cmd_draw_indexed_indirect_count =
                reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(resolve("vkCmdDrawIndexedIndirectCount"));
            if (!data.cmd_draw_indexed_indirect_count)
            {
                data.cmd_draw_indexed_indirect_count =
                    reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(resolve("vkCmdDrawIndexedIndirectCountKHR"));
            }
            data.cmd_draw_indirect = reinterpret_cast<PFN_vkCmdDrawIndirect>(resolve("vkCmdDrawIndirect"));
            data.cmd_draw_indirect_count = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(resolve("vkCmdDrawIndirectCount"));
            if (!data.cmd_draw_indirect_count)
            {
                data.cmd_draw_indirect_count = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(resolve("vkCmdDrawIndirectCountKHR"));
            }
            data.cmd_end_render_pass = reinterpret_cast<PFN_vkCmdEndRenderPass>(resolve("vkCmdEndRenderPass"));
            data.cmd_next_subpass = reinterpret_cast<PFN_vkCmdNextSubpass>(resolve("vkCmdNextSubpass"));
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
            data.cmd_set_depth_clip_enable = reinterpret_cast<PFN_vkCmdSetDepthClipEnableEXT>(resolve("vkCmdSetDepthClipEnableEXT"));
            data.cmd_set_stencil_test_enable = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(resolve("vkCmdSetStencilTestEnable"));
            data.cmd_set_rasterizer_discard_enable =
                reinterpret_cast<PFN_vkCmdSetRasterizerDiscardEnable>(resolve("vkCmdSetRasterizerDiscardEnable"));
            data.cmd_set_depth_bias_enable = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(resolve("vkCmdSetDepthBiasEnable"));
            data.cmd_set_primitive_restart_enable =
                reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(resolve("vkCmdSetPrimitiveRestartEnable"));
        }

        const uint64_t id = this->impl_->allocate_id();
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->queues.emplace(
            id,
            impl::queue_data{.handle = queue, .device_id = device, .queue_family_index = queue_family_index, .queue_index = queue_index});
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->command_pools.emplace(
            id, impl::command_pool_data{.handle = pool, .device_id = device, .queue_family_index = queue_family_index, .flags = flags});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_command_pool(uint64_t device, uint64_t pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->command_pools.find(pool);
        if (it == this->impl_->command_pools.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->command_pools.end() || pool_it->second.device_id != device ||
            !dev->second.allocate_command_buffers)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->command_buffers.emplace(
            id, impl::command_buffer_data{.handle = command_buffer, .device_id = device, .pool_id = pool, .level = level});
        out_command_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_command_buffer(uint64_t device, uint64_t pool, uint64_t command_buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->command_pools.find(pool);
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->command_pools.end() || pool_it->second.device_id != device ||
            cb == this->impl_->command_buffers.end() || cb->second.device_id != device || cb->second.pool_id != pool)
        {
            return;
        }

        if (cb->second.handle && dev->second.free_command_buffers)
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
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
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

        const VkResult result = dev->second.begin_command_buffer(cb->second.handle, &info);
        if (result == VK_SUCCESS)
        {
            cb->second.recorded.clear();
            cb->second.recorded_results.clear();
            cb->second.references.clear();
            cb->second.layout_transitions.clear();
            cb->second.query_resets.clear();
        }
        return result;
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
            if (it == this->impl_->command_buffers.end() || it->second.device_id != cb->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles.push_back(it->second.handle);
        }

        dev->second.cmd_execute_commands(cb->second.handle, static_cast<uint32_t>(handles.size()), handles.data());
        for (const uint64_t id : secondaries)
        {
            cb->second.layout_transitions.push_back(impl::layout_transition{.secondary = id});
        }
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
        if (it == this->impl_->command_pools.end() || it->second.device_id != device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult result = dev->second.reset_command_pool(dev->second.handle, it->second.handle, flags);
        if (result == VK_SUCCESS)
        {
            for (auto& command_buffer : this->impl_->command_buffers | std::views::values)
            {
                if (command_buffer.pool_id == pool)
                {
                    command_buffer.recorded.clear();
                    command_buffer.recorded_results.clear();
                    command_buffer.references.clear();
                    command_buffer.layout_transitions.clear();
                    command_buffer.query_resets.clear();
                }
            }
        }
        return result;
    }

    void vulkan_host::capture_references(const uint64_t command_buffer)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        this->impl_->recording_references = cb == this->impl_->command_buffers.end() ? nullptr : &cb->second.references;
    }

    void vulkan_host::append_recorded_command(const uint64_t command_buffer, const std::span<const std::byte> command, const int32_t result)
    {
        if (const auto cb = this->impl_->command_buffers.find(command_buffer); cb != this->impl_->command_buffers.end())
        {
            cb->second.recorded.insert(cb->second.recorded.end(), command.begin(), command.end());
            cb->second.recorded_results.push_back(result);
        }
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

        const VkResult result = dev->second.reset_command_buffer(cb->second.handle, flags);
        if (result == VK_SUCCESS)
        {
            cb->second.recorded.clear();
            cb->second.recorded_results.clear();
            cb->second.references.clear();
            cb->second.layout_transitions.clear();
            cb->second.query_resets.clear();
        }
        return result;
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

        const auto lock = this->impl_->queue_lock(queue_it->second.handle);
        const std::scoped_lock guard(*lock);
        return dev->second.queue_wait_idle(queue_it->second.handle);
    }

    int32_t vulkan_host::device_wait_idle(uint64_t device)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.device_wait_idle)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<std::shared_ptr<std::mutex>> locks{};
        for (const auto& queue : this->impl_->queues | std::views::values)
        {
            if (queue.device_id == device)
            {
                auto lock = this->impl_->queue_lock(queue.handle);
                if (std::ranges::find(locks, lock) == locks.end())
                {
                    locks.push_back(std::move(lock));
                }
            }
        }

        std::vector<std::unique_lock<std::mutex>> guards{};
        guards.reserve(locks.size());
        for (const auto& lock : locks)
        {
            guards.emplace_back(*lock);
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->fences.emplace(id, impl::fence_data{.handle = fence, .device_id = device});
        out_fence = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_fence(uint64_t device, uint64_t fence)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->fences.find(fence);
        if (it == this->impl_->fences.end() || it->second.device_id != device)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->semaphores.emplace(
            id, impl::semaphore_data{.handle = semaphore, .device_id = device, .flags = flags, .type = semaphore_type});
        out_semaphore = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_semaphore_counter_value(uint64_t device, uint64_t semaphore, uint64_t& out_value)
    {
        out_value = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->semaphores.find(semaphore);
        if (dev == this->impl_->devices.end() || it == this->impl_->semaphores.end() || it->second.device_id != device ||
            !dev->second.get_semaphore_counter_value)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        return dev->second.get_semaphore_counter_value(dev->second.handle, it->second.handle, &out_value);
    }

    int32_t vulkan_host::signal_semaphore(uint64_t device, uint64_t semaphore, uint64_t value)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->semaphores.find(semaphore);
        if (dev == this->impl_->devices.end() || it == this->impl_->semaphores.end() || it->second.device_id != device ||
            !dev->second.signal_semaphore)
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
            if (it == this->impl_->semaphores.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || it == this->impl_->buffers.end() || it->second.device_id != device ||
            !dev->second.get_buffer_device_address)
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
        if (it == this->impl_->semaphores.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || it == this->impl_->fences.end() || it->second.device_id != device ||
            !dev->second.reset_fences)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->events.emplace(id, impl::event_data{.handle = event, .device_id = device, .flags = flags});
        out_event = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_event(uint64_t device, uint64_t event)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->events.find(event);
        if (it == this->impl_->events.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || it == this->impl_->events.end() || it->second.device_id != device ||
            !dev->second.set_event)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.set_event(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::reset_event(uint64_t device, uint64_t event)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->events.find(event);
        if (dev == this->impl_->devices.end() || it == this->impl_->events.end() || it->second.device_id != device ||
            !dev->second.reset_event)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        return dev->second.reset_event(dev->second.handle, it->second.handle);
    }

    int32_t vulkan_host::queue_submit(uint64_t queue, uint64_t fence, const void* wait_entries, uint32_t wait_count,
                                      const void* command_buffer_ids, uint32_t command_buffer_count, const void* signal_entries,
                                      uint32_t signal_count, bool has_timeline_info)
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
            if (fence_it == this->impl_->fences.end() || fence_it->second.device_id != queue_it->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = fence_it->second.handle;
        }

        const auto build_semaphores = [&](const void* entries, uint32_t count, std::vector<VkSemaphore>& semaphores,
                                          std::vector<VkPipelineStageFlags>* stage_masks, std::vector<uint64_t>& values) {
            const auto* records = static_cast<const gpu_bridge::submit_semaphore_entry*>(entries);
            semaphores.reserve(count);
            if (stage_masks)
            {
                stage_masks->reserve(count);
            }
            values.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto sem = this->impl_->semaphores.find(records[i].semaphore);
                if (sem == this->impl_->semaphores.end() || sem->second.device_id != queue_it->second.device_id)
                {
                    return false;
                }
                semaphores.push_back(sem->second.handle);
                if (stage_masks)
                {
                    stage_masks->push_back(static_cast<VkPipelineStageFlags>(records[i].stage_mask));
                }
                values.push_back(records[i].value);
            }
            return true;
        };

        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> wait_stage_masks;
        std::vector<uint64_t> wait_values;
        std::vector<VkSemaphore> signals;
        std::vector<uint64_t> signal_values;
        if (!build_semaphores(wait_entries, wait_count, waits, &wait_stage_masks, wait_values) ||
            !build_semaphores(signal_entries, signal_count, signals, nullptr, signal_values))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto* command_ids = static_cast<const uint64_t*>(command_buffer_ids);
        std::vector<VkCommandBuffer> command_buffers;
        command_buffers.reserve(command_buffer_count);
        for (uint32_t i = 0; i < command_buffer_count; ++i)
        {
            const auto cb = this->impl_->command_buffers.find(command_ids[i]);
            if (cb == this->impl_->command_buffers.end() || cb->second.device_id != queue_it->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            command_buffers.push_back(cb->second.handle);
        }

        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
        timeline_info.pWaitSemaphoreValues = wait_values.data();
        timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
        timeline_info.pSignalSemaphoreValues = signal_values.data();

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = has_timeline_info ? &timeline_info : nullptr;
        submit.waitSemaphoreCount = wait_count;
        submit.pWaitSemaphores = waits.data();
        submit.pWaitDstStageMask = wait_stage_masks.data();
        submit.commandBufferCount = command_buffer_count;
        submit.pCommandBuffers = command_buffers.data();
        submit.signalSemaphoreCount = signal_count;
        submit.pSignalSemaphores = signals.data();

        const auto lock = this->impl_->queue_lock(queue_it->second.handle);
        const std::scoped_lock guard(*lock);
        const VkResult result = dev->second.queue_submit(queue_it->second.handle, 1, &submit, fence_handle);
        if (result == VK_SUCCESS)
        {
            this->impl_->apply_layout_transitions(queue_it->second.handle, command_ids, command_buffer_count);
            this->impl_->track_submitted_semaphores(static_cast<const gpu_bridge::submit_semaphore_entry*>(wait_entries), wait_count,
                                                    static_cast<const gpu_bridge::submit_semaphore_entry*>(signal_entries), signal_count);
        }
        return result;
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
            if (fence_it == this->impl_->fences.end() || fence_it->second.device_id != queue_it->second.device_id)
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
                if (sem == this->impl_->semaphores.end() || sem->second.device_id != queue_it->second.device_id)
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
            if (cb == this->impl_->command_buffers.end() || cb->second.device_id != queue_it->second.device_id)
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

        const auto lock = this->impl_->queue_lock(queue_it->second.handle);
        const std::scoped_lock guard(*lock);
        const VkResult result = dev->second.queue_submit2(queue_it->second.handle, 1, &submit, fence_handle);
        if (result == VK_SUCCESS)
        {
            this->impl_->apply_layout_transitions(queue_it->second.handle, cmd_ids, command_buffer_count);
            this->impl_->track_submitted_semaphores(static_cast<const gpu_bridge::submit2_semaphore_entry*>(wait_entries), wait_count,
                                                    static_cast<const gpu_bridge::submit2_semaphore_entry*>(signal_entries), signal_count);
        }
        return result;
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

    int32_t vulkan_host::get_physical_device_memory_budget(uint64_t physical_device, uint64_t* heap_budget, uint64_t* heap_usage,
                                                           uint32_t heap_capacity, uint32_t& out_heap_count)
    {
        out_heap_count = 0;

        const auto pd = this->impl_->physical_devices.find(physical_device);
        if (pd == this->impl_->physical_devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_physical_device_memory_properties2)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        properties2.pNext = &budget;

        instance->second.get_physical_device_memory_properties2(pd->second.handle, &properties2);

        const uint32_t count = std::min(properties2.memoryProperties.memoryHeapCount, heap_capacity);
        for (uint32_t i = 0; i < count; ++i)
        {
            heap_budget[i] = budget.heapBudget[i];
            heap_usage[i] = budget.heapUsage[i];
        }

        out_heap_count = count;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::allocate_memory(uint64_t device, uint64_t size, uint32_t memory_type_index, uint32_t flags, uint32_t device_mask,
                                         uint64_t& out_memory)
    {
        out_memory = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.allocate_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPhysicalDeviceMemoryProperties mem_props{};
        const bool have_props = this->impl_->query_memory_properties(dev->second, mem_props);
        // The spec requires memoryTypeIndex < memoryTypeCount; drivers index their internal type/heap arrays
        // with it unchecked, so an out-of-range guest value is an OOB read inside the host driver.
        if (have_props && memory_type_index >= mem_props.memoryTypeCount)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Round the allocation up to a whole number of guest pages. The map-direct path aliases the host
        // mapping straight into the guest, which can only be done at page granularity; rounding the actual
        // allocation up means the page-aligned alias never covers anything beyond this allocation, so the
        // tail of the final page is the guest's own (slack) memory rather than leaked host memory.
        constexpr uint64_t page_size = 0x1000;
        const uint64_t aligned_size = (size > UINT64_MAX - (page_size - 1)) ? size : page_align_up(size, page_size);

        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = aligned_size;
        info.memoryTypeIndex = this->impl_->substitute_cached_memory_type(dev->second, memory_type_index);

        VkMemoryAllocateFlagsInfo flags_info{};
        if (flags != 0 || device_mask != 0)
        {
            flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            flags_info.flags = flags;
            flags_info.deviceMask = device_mask;
            info.pNext = &flags_info;
        }

        VkDeviceMemory memory{};
        const VkResult result = dev->second.allocate_memory(dev->second.handle, &info, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        // Vulkan leaves VkDeviceMemory uninitialized. Host-visible memory is aliased/copied straight to the
        // guest (map_memory / download_memory), so zero it here -- otherwise the guest can read stale host
        // bytes from before it wrote anything (a host-process info leak).
        const VkMemoryPropertyFlags type_flags = (have_props && info.memoryTypeIndex < mem_props.memoryTypeCount)
                                                     ? mem_props.memoryTypes[info.memoryTypeIndex].propertyFlags
                                                     : 0;
        if ((type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && dev->second.map_memory && dev->second.unmap_memory)
        {
            void* zeroed = nullptr;
            if (dev->second.map_memory(dev->second.handle, memory, 0, VK_WHOLE_SIZE, 0, &zeroed) == VK_SUCCESS && zeroed)
            {
                std::memset(zeroed, 0, static_cast<size_t>(aligned_size));
                // Non-coherent memory needs an explicit flush for the zero-fill to reach the device and later
                // mappings; on coherent memory the write is already visible, so skip the extra call.
                if (!(type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) && dev->second.flush_mapped_memory_ranges)
                {
                    VkMappedMemoryRange range{};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = memory;
                    range.offset = 0;
                    range.size = VK_WHOLE_SIZE;
                    dev->second.flush_mapped_memory_ranges(dev->second.handle, 1, &range);
                }
                dev->second.unmap_memory(dev->second.handle, memory);
            }
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->memories.emplace(id, impl::memory_data{.handle = memory,
                                                            .device_id = device,
                                                            .requested_size = size,
                                                            .memory_type_index = memory_type_index,
                                                            .flags = flags,
                                                            .device_mask = device_mask,
                                                            .allocation_size = aligned_size});
        out_memory = id;
        return VK_SUCCESS;
    }

    void vulkan_host::free_memory(uint64_t device, uint64_t memory)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->memories.find(memory);
        // Only the owning device may free the memory; ignore a cross-device free request.
        if (it == this->impl_->memories.end() || it->second.device_id != device)
        {
            return;
        }

        if (dev != this->impl_->devices.end())
        {
            if (it->second.handle && dev->second.free_memory)
            {
                dev->second.free_memory(dev->second.handle, it->second.handle, nullptr);
            }
        }

        this->impl_->memories.erase(it);
    }

    int32_t vulkan_host::get_device_memory_commitment(uint64_t device, uint64_t memory, uint64_t& out_committed_bytes)
    {
        out_committed_bytes = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!dev->second.get_device_memory_commitment)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        VkDeviceSize committed = 0;
        dev->second.get_device_memory_commitment(dev->second.handle, mem->second.handle, &committed);
        out_committed_bytes = committed;
        return VK_SUCCESS;
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

        // Memory the bridge imports may only back resources created for its handle type.
        VkExternalMemoryBufferCreateInfo external_info{};
        if (this->impl_->buffer_supports_host_import(dev->second, info.flags, usage))
        {
            external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            info.pNext = &external_info;
        }

        VkBuffer buffer{};
        const VkResult result = dev->second.create_buffer(dev->second.handle, &info, nullptr, &buffer);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->buffers.emplace(id, impl::buffer_data{.handle = buffer, .device_id = device, .usage = usage, .size = size});
        out_buffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_buffer(uint64_t device, uint64_t buffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffers.find(buffer);
        if (it == this->impl_->buffers.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || buf->second.device_id != device ||
            !dev->second.get_buffer_memory_requirements)
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
            buf->second.device_id != device || mem->second.device_id != device || !dev->second.bind_buffer_memory)
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
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
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
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.map_memory || !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!is_memory_range_valid(mem->second.allocation_size, offset, size, false))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), out_size);

        // A VkDeviceMemory must not be mapped twice, so memory the guest keeps mapped is accessed through that mapping.
        if (mem->second.persistent_host_pointer)
        {
            auto* mapped = static_cast<std::byte*>(mem->second.persistent_host_pointer) + offset;
            std::memcpy(out, mapped, copy_bytes);
            return VK_SUCCESS;
        }

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
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.map_memory || !dev->second.unmap_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!is_memory_range_valid(mem->second.allocation_size, offset, size, false))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        const size_t copy_bytes = std::min(static_cast<size_t>(size), data_size);

        // A VkDeviceMemory must not be mapped twice, so memory the guest keeps mapped is accessed through that mapping.
        if (mem->second.persistent_host_pointer)
        {
            auto* mapped = static_cast<std::byte*>(mem->second.persistent_host_pointer) + offset;
            std::memcpy(mapped, data, copy_bytes);
            return VK_SUCCESS;
        }

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
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.flush_mapped_memory_ranges)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!is_memory_range_valid(mem->second.allocation_size, offset, size, true))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
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
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.invalidate_mapped_memory_ranges)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!is_memory_range_valid(mem->second.allocation_size, offset, size, true))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
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
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.map_memory)
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

    bool vulkan_host::has_swapchains() const
    {
        return !this->impl_->swapchains.empty();
    }

    bool vulkan_host::has_device(const uint64_t device) const
    {
        return this->impl_->devices.contains(device);
    }

    uint64_t vulkan_host::placed_map_alignment(uint64_t device) const
    {
        const auto dev = this->impl_->devices.find(device);
        return dev == this->impl_->devices.end() ? 0 : dev->second.placed_map_alignment;
    }

    uint64_t vulkan_host::host_import_alignment(uint64_t device) const
    {
        const auto dev = this->impl_->devices.find(device);
        return dev == this->impl_->devices.end() ? 0 : dev->second.host_import_alignment;
    }

    bool vulkan_host::is_host_visible_memory_type(uint64_t device, uint32_t memory_type_index) const
    {
        const auto dev = this->impl_->devices.find(device);
        VkPhysicalDeviceMemoryProperties mem_props{};
        if (dev == this->impl_->devices.end() || !this->impl_->query_memory_properties(dev->second, mem_props) ||
            memory_type_index >= mem_props.memoryTypeCount)
        {
            return false;
        }

        const auto type_index = this->impl_->substitute_cached_memory_type(dev->second, memory_type_index);
        return (mem_props.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    }

    int32_t vulkan_host::import_host_memory(uint64_t device, uint64_t size, uint32_t memory_type_index, uint32_t flags,
                                            uint32_t device_mask, void* host_pointer, uint64_t& out_memory)
    {
        out_memory = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || dev->second.host_import_alignment == 0 || !dev->second.allocate_memory ||
            size % dev->second.host_import_alignment != 0 ||
            reinterpret_cast<uintptr_t>(host_pointer) % dev->second.host_import_alignment != 0)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto type_index = this->impl_->substitute_cached_memory_type(dev->second, memory_type_index);

        VkMemoryHostPointerPropertiesEXT pointer_properties{};
        pointer_properties.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
        const VkResult properties_result = dev->second.get_memory_host_pointer_properties(
            dev->second.handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_pointer, &pointer_properties);
        // Drivers may accept host pointers only for some memory types, e.g. Mali only for its cached, non-coherent one,
        // not for the coherent one the guest picked. Another host-visible type of the same heap stands in for it.
        auto import_type_index = type_index;
        if (properties_result == VK_SUCCESS && (pointer_properties.memoryTypeBits & (1u << type_index)) == 0)
        {
            VkPhysicalDeviceMemoryProperties mem_props{};
            this->impl_->query_memory_properties(dev->second, mem_props);
            for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
            {
                if ((pointer_properties.memoryTypeBits & (1u << i)) != 0 &&
                    (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
                    mem_props.memoryTypes[i].heapIndex == mem_props.memoryTypes[type_index].heapIndex)
                {
                    import_type_index = i;
                    break;
                }
            }
        }

        if (properties_result != VK_SUCCESS || (pointer_properties.memoryTypeBits & (1u << import_type_index)) == 0)
        {
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }

        VkImportMemoryHostPointerInfoEXT import_info{.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                     .pNext = nullptr,
                                                     .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     .pHostPointer = host_pointer};

        VkMemoryAllocateFlagsInfo flags_info{};
        if (flags != 0 || device_mask != 0)
        {
            flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            flags_info.flags = flags;
            flags_info.deviceMask = device_mask;
            import_info.pNext = &flags_info;
        }

        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.pNext = &import_info;
        info.allocationSize = size;
        info.memoryTypeIndex = import_type_index;

        VkDeviceMemory memory{};
        const VkResult result = dev->second.allocate_memory(dev->second.handle, &info, nullptr, &memory);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        // Flushing and invalidating require a vkMapMemory mapping. The guest keeps using host_pointer; the CPU caches are
        // physically tagged, so maintaining them through either mapping covers both.
        VkPhysicalDeviceMemoryProperties import_properties{};
        this->impl_->query_memory_properties(dev->second, import_properties);
        const bool non_coherent =
            (import_properties.memoryTypes[import_type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
        if (non_coherent)
        {
            void* mapped = nullptr;
            if (!dev->second.map_memory || !dev->second.flush_mapped_memory_ranges || !dev->second.invalidate_mapped_memory_ranges ||
                dev->second.map_memory(dev->second.handle, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            {
                dev->second.free_memory(dev->second.handle, memory, nullptr);
                return VK_ERROR_MEMORY_MAP_FAILED;
            }
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->memories.emplace(id, impl::memory_data{.handle = memory,
                                                            .device_id = device,
                                                            .requested_size = size,
                                                            .memory_type_index = memory_type_index,
                                                            .flags = flags,
                                                            .device_mask = device_mask,
                                                            .persistent_host_pointer = host_pointer,
                                                            .mapped_size = size,
                                                            .allocation_size = size,
                                                            .imported = true,
                                                            .non_coherent = non_coherent});
        out_memory = id;
        return VK_SUCCESS;
    }

    bool vulkan_host::is_non_coherent_import(const uint64_t memory) const
    {
        const auto mem = this->impl_->memories.find(memory);
        return mem != this->impl_->memories.end() && mem->second.imported && mem->second.non_coherent;
    }

    void vulkan_host::flush_imported_memory(const std::span<const memory_range> ranges)
    {
        for (const auto& [device_id, mapped_ranges] : this->impl_->to_mapped_ranges(ranges))
        {
            const auto dev = this->impl_->devices.find(device_id);
            if (dev != this->impl_->devices.end() && dev->second.flush_mapped_memory_ranges)
            {
                dev->second.flush_mapped_memory_ranges(dev->second.handle, static_cast<uint32_t>(mapped_ranges.size()),
                                                       mapped_ranges.data());
            }
        }
    }

    void vulkan_host::invalidate_imported_memory(const std::span<const memory_range> ranges)
    {
        for (const auto& [device_id, mapped_ranges] : this->impl_->to_mapped_ranges(ranges))
        {
            const auto dev = this->impl_->devices.find(device_id);
            if (dev != this->impl_->devices.end() && dev->second.invalidate_mapped_memory_ranges)
            {
                dev->second.invalidate_mapped_memory_ranges(dev->second.handle, static_cast<uint32_t>(mapped_ranges.size()),
                                                            mapped_ranges.data());
            }
        }
    }

    uint64_t vulkan_host::allocation_size(uint64_t memory) const
    {
        const auto mem = this->impl_->memories.find(memory);
        return mem == this->impl_->memories.end() ? 0 : mem->second.allocation_size;
    }

    int32_t vulkan_host::map_memory_placed(uint64_t device, uint64_t memory, void* address)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            dev->second.placed_map_alignment == 0 || mem->second.persistent_host_pointer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (reinterpret_cast<uintptr_t>(address) % dev->second.placed_map_alignment != 0)
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        VkMemoryMapPlacedInfoEXT placed_info{};
        placed_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT;
        placed_info.pPlacedAddress = address;

        VkMemoryMapInfoKHR map_info{};
        map_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR;
        map_info.pNext = &placed_info;
        map_info.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
        map_info.memory = mem->second.handle;
        map_info.offset = 0;
        map_info.size = VK_WHOLE_SIZE;

        void* mapped = nullptr;
        const VkResult result = dev->second.map_memory2(dev->second.handle, &map_info, &mapped);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        mem->second.persistent_host_pointer = mapped;
        mem->second.mapped_size = mem->second.allocation_size;
        mem->second.placed = true;
        return VK_SUCCESS;
    }

    void vulkan_host::unmap_memory(uint64_t device, uint64_t memory)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto mem = this->impl_->memories.find(memory);
        if (dev == this->impl_->devices.end() || mem == this->impl_->memories.end() || mem->second.device_id != device ||
            !dev->second.unmap_memory)
        {
            return;
        }

        if (mem->second.imported)
        {
            return;
        }

        if (mem->second.placed)
        {
            VkMemoryUnmapInfoKHR unmap_info{};
            unmap_info.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR;
            unmap_info.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
            unmap_info.memory = mem->second.handle;
            // The driver may fail to keep the range reserved and leave it mapped. The memory must not stay mapped, since
            // the caller releases the range.
            if (dev->second.unmap_memory2(dev->second.handle, &unmap_info) != VK_SUCCESS)
            {
                dev->second.unmap_memory(dev->second.handle, mem->second.handle);
            }
            mem->second.placed = false;
        }
        else
        {
            dev->second.unmap_memory(dev->second.handle, mem->second.handle);
        }
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

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
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

        constexpr VkImageUsageFlags transfer_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        bool copyable = (info.usage & transfer_usage) == transfer_usage;
        if (!copyable)
        {
            auto with_transfer = info;
            with_transfer.usage |= transfer_usage;
            if (this->impl_->image_usage_supported(dev->second, with_transfer) &&
                this->impl_->image_supports_host_import(dev->second, with_transfer) ==
                    this->impl_->image_supports_host_import(dev->second, info))
            {
                info.usage = with_transfer.usage;
                copyable = true;
            }
        }

        VkExternalMemoryImageCreateInfo external_info{};
        if (this->impl_->image_supports_host_import(dev->second, info))
        {
            external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            info.pNext = &external_info;
        }

        VkImage image{};
        const VkResult result = dev->second.create_image(dev->second.handle, &info, nullptr, &image);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        impl::image_data data{.handle = image, .device_id = device, .samples = samples ? samples : 1, .copyable = copyable};
        data.create = impl::image_create_params{
            .format = format,
            .width = width,
            .height = height,
            .depth = info.extent.depth,
            .mip_levels = info.mipLevels,
            .array_layers = info.arrayLayers,
            .samples = samples,
            .usage = usage,
            .tiling = tiling,
            .image_type = image_type,
            .flags = flags,
        };
        data.layouts.assign(static_cast<size_t>(data.aspect_count()) * info.mipLevels * info.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED);
        this->impl_->images.emplace(id, std::move(data));
        out_image = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image(uint64_t device, uint64_t image)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->images.find(image);
        if (it == this->impl_->images.end() || it->second.device_id != device || !it->second.owned)
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
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || img->second.device_id != device ||
            !dev->second.get_image_memory_requirements)
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
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || img->second.device_id != device ||
            !dev->second.get_image_subresource_layout)
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
            img->second.device_id != device || mem->second.device_id != device || !dev->second.bind_image_memory)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const VkResult result = dev->second.bind_image_memory(dev->second.handle, img->second.handle, mem->second.handle, offset);
        if (result == VK_SUCCESS)
        {
            img->second.memory_id = memory;
            img->second.memory_offset = offset;
        }
        return result;
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

        VkImageLayout translate_layout(uint32_t layout)
        {
            return static_cast<VkImageLayout>(layout);
        }
    }

    int32_t vulkan_host::cmd_pipeline_barrier(uint64_t command_buffer, uint64_t image, uint32_t src_stage_mask, uint32_t dst_stage_mask,
                                              uint32_t src_access_mask, uint32_t dst_access_mask, uint32_t old_layout, uint32_t new_layout,
                                              uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
                                              const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() || img->second.device_id != cb->second.device_id)
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
        barrier.oldLayout = img->second.layout_undefined ? VK_IMAGE_LAYOUT_UNDEFINED : translate_layout(old_layout);
        barrier.newLayout = translate_layout(new_layout);
        barrier.srcQueueFamilyIndex = src_queue_family_index;
        barrier.dstQueueFamilyIndex = dst_queue_family_index;
        barrier.image = img->second.handle;
        barrier.subresourceRange = to_vk_range(range);

        dev->second.cmd_pipeline_barrier(cb->second.handle, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        cb->second.layout_transitions.push_back(impl::layout_transition{
            .image = image,
            .aspect_mask = range.aspect_mask,
            .base_mip_level = range.base_mip_level,
            .level_count = range.level_count,
            .base_array_layer = range.base_array_layer,
            .layer_count = range.layer_count,
            .new_layout = static_cast<uint32_t>(barrier.newLayout),
            .transfers_ownership = src_queue_family_index != dst_queue_family_index && src_queue_family_index != VK_QUEUE_FAMILY_IGNORED,
        });
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_clear_color_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float r, float g, float b,
                                               float a, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() || img->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_clear_attachments(uint64_t command_buffer, uint32_t attachment_count, uint32_t rect_count, const void* data,
                                               size_t data_size)
    {
        const size_t attach_bytes = static_cast<size_t>(attachment_count) * sizeof(VkClearAttachment);
        const size_t rect_bytes = static_cast<size_t>(rect_count) * sizeof(VkClearRect);
        if (data_size < attach_bytes || data_size - attach_bytes < rect_bytes)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_clear_attachments)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // VkClearAttachment / VkClearRect carry no object handles, so the guest blob is forwarded verbatim.
        const auto* attachments = reinterpret_cast<const VkClearAttachment*>(data);
        const auto* rects = reinterpret_cast<const VkClearRect*>(static_cast<const uint8_t*>(data) + attach_bytes);
        dev->second.cmd_clear_attachments(cb->second.handle, attachment_count, attachment_count ? attachments : nullptr, rect_count,
                                          rect_count ? rects : nullptr);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_clear_depth_stencil_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float depth,
                                                       uint32_t stencil, const subresource_range& range)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() || img->second.device_id != cb->second.device_id)
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
                                                  const buffer_image_copy_region& r)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto img = this->impl_->images.find(image);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || img == this->impl_->images.end() || buf == this->impl_->buffers.end() ||
            img->second.device_id != cb->second.device_id || buf->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_image_to_buffer)
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

        dev->second.cmd_copy_image_to_buffer(cb->second.handle, img->second.handle, translate_layout(image_layout), buf->second.handle, 1,
                                             &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_resolve_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image,
                                           uint32_t dst_layout, const image_copy_region& r)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto src = this->impl_->images.find(src_image);
        const auto dst = this->impl_->images.find(dst_image);
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->images.end() || dst == this->impl_->images.end() ||
            src->second.device_id != cb->second.device_id || dst->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_resolve_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageResolve region{};
        region.srcSubresource = {.aspectMask = r.src_aspect_mask,
                                 .mipLevel = r.src_mip_level,
                                 .baseArrayLayer = r.src_base_array_layer,
                                 .layerCount = r.src_layer_count};
        region.srcOffset = {.x = r.src_offset_x, .y = r.src_offset_y, .z = r.src_offset_z};
        region.dstSubresource = {.aspectMask = r.dst_aspect_mask,
                                 .mipLevel = r.dst_mip_level,
                                 .baseArrayLayer = r.dst_base_array_layer,
                                 .layerCount = r.dst_layer_count};
        region.dstOffset = {.x = r.dst_offset_x, .y = r.dst_offset_y, .z = r.dst_offset_z};
        region.extent = {.width = r.width, .height = r.height, .depth = r.depth};

        dev->second.cmd_resolve_image(cb->second.handle, src->second.handle, translate_layout(src_layout), dst->second.handle,
                                      translate_layout(dst_layout), 1, &region);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_update_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, const void* data, uint32_t size)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
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
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || img == this->impl_->images.end() ||
            buf->second.device_id != cb->second.device_id || img->second.device_id != cb->second.device_id)
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
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->images.end() || dst == this->impl_->images.end() ||
            src->second.device_id != cb->second.device_id || dst->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_blit_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image,
                                        uint32_t dst_layout, const image_blit_region& r)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto src = this->impl_->images.find(src_image);
        const auto dst = this->impl_->images.find(dst_image);
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->images.end() || dst == this->impl_->images.end() ||
            src->second.device_id != cb->second.device_id || dst->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_blit_image)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkImageBlit region{};
        region.srcSubresource = {.aspectMask = r.src_aspect_mask,
                                 .mipLevel = r.src_mip_level,
                                 .baseArrayLayer = r.src_base_array_layer,
                                 .layerCount = r.src_layer_count ? r.src_layer_count : 1};
        region.srcOffsets[0] = {.x = r.src_offset_x0, .y = r.src_offset_y0, .z = r.src_offset_z0};
        region.srcOffsets[1] = {.x = r.src_offset_x1, .y = r.src_offset_y1, .z = r.src_offset_z1};
        region.dstSubresource = {.aspectMask = r.dst_aspect_mask,
                                 .mipLevel = r.dst_mip_level,
                                 .baseArrayLayer = r.dst_base_array_layer,
                                 .layerCount = r.dst_layer_count ? r.dst_layer_count : 1};
        region.dstOffsets[0] = {.x = r.dst_offset_x0, .y = r.dst_offset_y0, .z = r.dst_offset_z0};
        region.dstOffsets[1] = {.x = r.dst_offset_x1, .y = r.dst_offset_y1, .z = r.dst_offset_z1};

        dev->second.cmd_blit_image(cb->second.handle, src->second.handle, translate_layout(src_layout), dst->second.handle,
                                   translate_layout(dst_layout), 1, &region, static_cast<VkFilter>(r.filter));
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_sampler(const uint64_t device, const sampler_desc& desc, uint64_t& out_sampler)
    {
        out_sampler = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_sampler)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.flags = desc.flags;
        info.magFilter = static_cast<VkFilter>(desc.mag_filter);
        info.minFilter = static_cast<VkFilter>(desc.min_filter);
        info.mipmapMode = static_cast<VkSamplerMipmapMode>(desc.mipmap_mode);
        info.addressModeU = static_cast<VkSamplerAddressMode>(desc.address_mode_u);
        info.addressModeV = static_cast<VkSamplerAddressMode>(desc.address_mode_v);
        info.addressModeW = static_cast<VkSamplerAddressMode>(desc.address_mode_w);
        info.mipLodBias = desc.mip_lod_bias;
        info.anisotropyEnable = desc.anisotropy_enable ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = desc.max_anisotropy < 1.0f ? 1.0f : desc.max_anisotropy;
        // Depth-compare / PCF sampling (sampler2DShadow): a non-comparison sampler yields undefined results
        // here, which is what made shadow-mapped sun lighting render as fully shadowed.
        info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
        info.compareOp = static_cast<VkCompareOp>(desc.compare_op);
        info.minLod = desc.min_lod;
        info.maxLod = desc.max_lod;
        info.borderColor = static_cast<VkBorderColor>(desc.border_color);
        info.unnormalizedCoordinates = desc.unnormalized_coordinates ? VK_TRUE : VK_FALSE;

        VkSamplerReductionModeCreateInfo reduction{};
        if (desc.reduction_mode)
        {
            reduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
            reduction.reductionMode = static_cast<VkSamplerReductionMode>(*desc.reduction_mode);
            reduction.pNext = info.pNext;
            info.pNext = &reduction;
        }

        VkSamplerCustomBorderColorCreateInfoEXT custom_border{};
        if (desc.custom_border_color)
        {
            custom_border.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
            custom_border.format = static_cast<VkFormat>(desc.custom_border_color->first);
            std::memcpy(&custom_border.customBorderColor, desc.custom_border_color->second.data(), sizeof(custom_border.customBorderColor));
            custom_border.pNext = info.pNext;
            info.pNext = &custom_border;
        }

        VkSampler sampler{};
        const VkResult result = dev->second.create_sampler(dev->second.handle, &info, nullptr, &sampler);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->samplers.emplace(id, impl::sampler_data{.handle = sampler, .device_id = device, .create = desc});
        out_sampler = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_sampler(uint64_t device, uint64_t sampler)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->samplers.find(sampler);
        if (it == this->impl_->samplers.end() || it->second.device_id != device)
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_sampler)
        {
            dev->second.destroy_sampler(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->samplers.erase(it);
    }

    int32_t vulkan_host::create_surface(uint64_t instance, uint64_t native_surface, uint64_t& out_surface)
    {
        out_surface = 0;
        if (native_surface == 0 || !this->impl_->instances.contains(instance))
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->surfaces.emplace(id, impl::surface_data{
                                              .handle = unpack_vulkan_handle<VkSurfaceKHR>(native_surface),
                                              .instance_id = instance,
                                          });
        out_surface = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_surface(uint64_t surface)
    {
        const auto it = this->impl_->surfaces.find(surface);
        if (it == this->impl_->surfaces.end())
        {
            return;
        }

        const auto instance = this->impl_->instances.find(it->second.instance_id);
        if (instance != this->impl_->instances.end() && it->second.handle && instance->second.destroy_surface)
        {
            instance->second.destroy_surface(instance->second.handle, it->second.handle, nullptr);
        }
        this->impl_->surfaces.erase(it);
    }

    int32_t vulkan_host::get_surface_support(uint64_t physical_device, uint64_t surface, uint32_t queue_family_index,
                                             uint32_t& out_supported)
    {
        out_supported = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        const auto surf = this->impl_->surfaces.find(surface);
        if (pd == this->impl_->physical_devices.end() || surf == this->impl_->surfaces.end() ||
            pd->second.instance_id != surf->second.instance_id)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_support)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkBool32 supported = VK_FALSE;
        const VkResult result =
            instance->second.get_surface_support(pd->second.handle, queue_family_index, surf->second.handle, &supported);
        out_supported = supported == VK_TRUE ? 1u : 0u;
        return result;
    }

    int32_t vulkan_host::get_surface_capabilities(uint64_t physical_device, uint64_t surface, void* out, size_t out_size,
                                                  const std::optional<std::pair<uint32_t, uint32_t>> current_extent)
    {
        const auto pd = this->impl_->physical_devices.find(physical_device);
        const auto surf = this->impl_->surfaces.find(surface);
        if (pd == this->impl_->physical_devices.end() || surf == this->impl_->surfaces.end() ||
            pd->second.instance_id != surf->second.instance_id)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_capabilities)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkSurfaceCapabilitiesKHR caps{};
        const VkResult result = instance->second.get_surface_capabilities(pd->second.handle, surf->second.handle, &caps);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        if (current_extent)
        {
            caps.currentExtent.width = std::clamp(current_extent->first, caps.minImageExtent.width, caps.maxImageExtent.width);
            caps.currentExtent.height = std::clamp(current_extent->second, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (out && out_size > 0)
        {
            std::memcpy(out, &caps, std::min(out_size, sizeof(caps)));
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_surface_formats(uint64_t physical_device, uint64_t surface, std::span<surface_format> out_formats,
                                             uint32_t& out_count)
    {
        out_count = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        const auto surf = this->impl_->surfaces.find(surface);
        if (pd == this->impl_->physical_devices.end() || surf == this->impl_->surfaces.end() ||
            pd->second.instance_id != surf->second.instance_id)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_formats)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        uint32_t count = 0;
        VkResult result = instance->second.get_surface_formats(pd->second.handle, surf->second.handle, &count, nullptr);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        out_count = count;
        if (out_formats.empty() || count == 0)
        {
            return VK_SUCCESS;
        }

        const uint32_t capacity = static_cast<uint32_t>(std::min<size_t>(out_formats.size(), count));
        std::vector<VkSurfaceFormatKHR> formats(capacity);
        uint32_t written = capacity;
        result = instance->second.get_surface_formats(pd->second.handle, surf->second.handle, &written, formats.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return result;
        }
        for (uint32_t i = 0; i < written; ++i)
        {
            out_formats[i] = {
                .format = static_cast<uint32_t>(formats[i].format),
                .color_space = static_cast<uint32_t>(formats[i].colorSpace),
            };
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_surface_present_modes(uint64_t physical_device, uint64_t surface, std::span<uint32_t> out_modes,
                                                   uint32_t& out_count)
    {
        out_count = 0;
        const auto pd = this->impl_->physical_devices.find(physical_device);
        const auto surf = this->impl_->surfaces.find(surface);
        if (pd == this->impl_->physical_devices.end() || surf == this->impl_->surfaces.end() ||
            pd->second.instance_id != surf->second.instance_id)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_present_modes)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        uint32_t count = 0;
        VkResult result = instance->second.get_surface_present_modes(pd->second.handle, surf->second.handle, &count, nullptr);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        out_count = count;
        if (out_modes.empty() || count == 0)
        {
            return VK_SUCCESS;
        }

        const uint32_t capacity = static_cast<uint32_t>(std::min<size_t>(out_modes.size(), count));
        std::vector<VkPresentModeKHR> modes(capacity);
        uint32_t written = capacity;
        result = instance->second.get_surface_present_modes(pd->second.handle, surf->second.handle, &written, modes.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return result;
        }
        for (uint32_t i = 0; i < written; ++i)
        {
            out_modes[i] = static_cast<uint32_t>(modes[i]);
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_swapchain(uint64_t device, uint64_t surface, uint64_t old_swapchain, uint32_t flags, uint32_t format,
                                          uint32_t color_space, uint32_t width, uint32_t height, uint32_t image_array_layers,
                                          uint32_t min_image_count, uint32_t image_usage, uint32_t sharing_mode,
                                          std::span<const uint32_t> queue_family_indices, uint32_t pre_transform, uint32_t composite_alpha,
                                          uint32_t present_mode, uint32_t clipped, uint64_t& out_swapchain, uint32_t& out_image_count)
    {
        out_swapchain = 0;
        out_image_count = 0;

        const auto dev_it = this->impl_->devices.find(device);
        const auto surf_it = this->impl_->surfaces.find(surface);
        if (dev_it == this->impl_->devices.end() || surf_it == this->impl_->surfaces.end() ||
            dev_it->second.instance_id != surf_it->second.instance_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        impl::device_data& dev = dev_it->second;
        if (!dev.create_swapchain || !dev.destroy_swapchain || !dev.get_swapchain_images)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkSwapchainKHR native_old_swapchain = VK_NULL_HANDLE;
        if (old_swapchain != 0)
        {
            const auto old = this->impl_->swapchains.find(old_swapchain);
            if (old == this->impl_->swapchains.end() || old->second.device_id != device || old->second.surface_id != surface)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            native_old_swapchain = old->second.handle;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.flags = static_cast<VkSwapchainCreateFlagsKHR>(flags);
        info.surface = surf_it->second.handle;
        info.minImageCount = min_image_count;
        info.imageFormat = static_cast<VkFormat>(format);
        info.imageColorSpace = static_cast<VkColorSpaceKHR>(color_space);
        info.imageExtent = {.width = width, .height = height};
        info.imageArrayLayers = image_array_layers;
        info.imageUsage = static_cast<VkImageUsageFlags>(image_usage);
        info.imageSharingMode = static_cast<VkSharingMode>(sharing_mode);
        info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_indices.size());
        info.pQueueFamilyIndices = queue_family_indices.empty() ? nullptr : queue_family_indices.data();
        info.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(pre_transform);
        info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(composite_alpha);
        info.presentMode = static_cast<VkPresentModeKHR>(present_mode);
        info.clipped = clipped ? VK_TRUE : VK_FALSE;
        info.oldSwapchain = native_old_swapchain;

        VkSwapchainKHR native_swapchain{};
        VkResult result = dev.create_swapchain(dev.handle, &info, nullptr, &native_swapchain);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        uint32_t image_count = 0;
        result = dev.get_swapchain_images(dev.handle, native_swapchain, &image_count, nullptr);
        if (result != VK_SUCCESS || image_count == 0)
        {
            dev.destroy_swapchain(dev.handle, native_swapchain, nullptr);
            return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
        }

        std::vector<VkImage> native_images(image_count);
        result = dev.get_swapchain_images(dev.handle, native_swapchain, &image_count, native_images.data());
        if (result != VK_SUCCESS)
        {
            dev.destroy_swapchain(dev.handle, native_swapchain, nullptr);
            return result;
        }
        native_images.resize(image_count);

        impl::swapchain_data sc{};
        sc.handle = native_swapchain;
        sc.device_id = device;
        sc.surface_id = surface;
        sc.create = impl::swapchain_params{.flags = flags,
                                           .format = format,
                                           .color_space = color_space,
                                           .width = width,
                                           .height = height,
                                           .image_array_layers = image_array_layers,
                                           .min_image_count = min_image_count,
                                           .image_usage = image_usage,
                                           .sharing_mode = sharing_mode,
                                           .queue_family_indices = {queue_family_indices.begin(), queue_family_indices.end()},
                                           .pre_transform = pre_transform,
                                           .composite_alpha = composite_alpha,
                                           .present_mode = present_mode,
                                           .clipped = clipped};
        sc.acquired.assign(native_images.size(), false);
        sc.image_ids.reserve(native_images.size());
        for (VkImage image : native_images)
        {
            const uint64_t image_id = this->impl_->allocate_id();
            impl::image_data data{.handle = image, .device_id = device, .samples = 1, .owned = false};
            data.create = impl::image_create_params{.format = format,
                                                    .width = width,
                                                    .height = height,
                                                    .depth = 1,
                                                    .mip_levels = 1,
                                                    .array_layers = std::max(1U, image_array_layers),
                                                    .samples = 1,
                                                    .usage = image_usage,
                                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                    .image_type = VK_IMAGE_TYPE_2D,
                                                    .flags = 0};
            data.layouts.assign(data.create.array_layers, VK_IMAGE_LAYOUT_UNDEFINED);
            this->impl_->images.emplace(image_id, std::move(data));
            sc.image_ids.push_back(image_id);
        }

        const uint64_t id = this->impl_->allocate_id();
        out_image_count = static_cast<uint32_t>(sc.image_ids.size());
        this->impl_->swapchains.emplace(id, std::move(sc));
        out_swapchain = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_swapchain(uint64_t device, uint64_t swapchain)
    {
        const auto it = this->impl_->swapchains.find(swapchain);
        if (it == this->impl_->swapchains.end() || it->second.device_id != device)
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

    int32_t vulkan_host::acquire_next_image(uint64_t swapchain, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t& out_index)
    {
        out_index = 0;
        const auto sc_it = this->impl_->swapchains.find(swapchain);
        if (sc_it == this->impl_->swapchains.end())
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        const auto dev_it = this->impl_->devices.find(sc_it->second.device_id);
        if (dev_it == this->impl_->devices.end() || !dev_it->second.acquire_next_image)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        VkSemaphore semaphore_handle = VK_NULL_HANDLE;
        if (semaphore != 0)
        {
            const auto it = this->impl_->semaphores.find(semaphore);
            if (it == this->impl_->semaphores.end() || it->second.device_id != sc_it->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            semaphore_handle = it->second.handle;
        }

        VkFence fence_handle = VK_NULL_HANDLE;
        if (fence != 0)
        {
            const auto it = this->impl_->fences.find(fence);
            if (it == this->impl_->fences.end() || it->second.device_id != sc_it->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            fence_handle = it->second.handle;
        }

        const VkResult result = dev_it->second.acquire_next_image(dev_it->second.handle, sc_it->second.handle, timeout, semaphore_handle,
                                                                  fence_handle, &out_index);
        if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && out_index < sc_it->second.acquired.size())
        {
            sc_it->second.acquired[out_index] = true;
            if (semaphore != 0)
            {
                this->impl_->semaphores.at(semaphore).binary_signaled = true;
            }
        }
        return result;
    }

    int32_t vulkan_host::queue_present(uint64_t queue, uint64_t swapchain, uint32_t image_index,
                                       const std::vector<uint64_t>& wait_semaphores,
                                       const std::function<int32_t(const std::function<int32_t()>&)>& run_unlocked)
    {
        const auto queue_it = this->impl_->queues.find(queue);
        const auto sc_it = this->impl_->swapchains.find(swapchain);
        if (queue_it == this->impl_->queues.end() || sc_it == this->impl_->swapchains.end() ||
            queue_it->second.device_id != sc_it->second.device_id || image_index >= sc_it->second.image_ids.size())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev_it = this->impl_->devices.find(sc_it->second.device_id);
        if (dev_it == this->impl_->devices.end() || !dev_it->second.queue_present)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        std::vector<VkSemaphore> wait_handles;
        wait_handles.reserve(wait_semaphores.size());
        for (const uint64_t semaphore : wait_semaphores)
        {
            const auto it = this->impl_->semaphores.find(semaphore);
            if (it == this->impl_->semaphores.end() || it->second.device_id != sc_it->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            wait_handles.push_back(it->second.handle);
        }

        VkSwapchainKHR native_swapchain = sc_it->second.handle;
        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = static_cast<uint32_t>(wait_handles.size());
        info.pWaitSemaphores = wait_handles.empty() ? nullptr : wait_handles.data();
        info.swapchainCount = 1;
        info.pSwapchains = &native_swapchain;
        info.pImageIndices = &image_index;

        const auto native_present = dev_it->second.queue_present;
        auto* const native_queue = queue_it->second.handle;
        const auto queue_lock = this->impl_->queue_lock(native_queue);
        const auto presents = dev_it->second.presents;
        std::shared_lock device_in_use(*presents);

        // The device lock is released before run_unlocked returns, since its caller may then wait for a lock that a
        // thread destroying the device holds.
        const int32_t result = run_unlocked([&] {
            int32_t present_result{};
            {
                const std::scoped_lock guard(*queue_lock);
                present_result = native_present(native_queue, &info);
            }
            device_in_use.unlock();
            return present_result;
        });

        // Only these leave the present unqueued, with the image still acquired and the semaphores untouched.
        if (result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
        {
            if (const auto presented = this->impl_->swapchains.find(swapchain); presented != this->impl_->swapchains.end())
            {
                presented->second.acquired[image_index] = false;
            }

            for (const uint64_t semaphore : wait_semaphores)
            {
                if (const auto waited = this->impl_->semaphores.find(semaphore); waited != this->impl_->semaphores.end())
                {
                    waited->second.binary_signaled = false;
                }
            }
        }
        return result;
    }

    int32_t vulkan_host::create_shader_module(uint64_t device, uint32_t flags, const void* code, size_t code_size, uint64_t& out_module)
    {
        out_module = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_shader_module)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.flags = static_cast<VkShaderModuleCreateFlags>(flags);
        info.codeSize = code_size;
        info.pCode = static_cast<const uint32_t*>(code);

        // Mali's compiler crashes on clip and cull distance writes, which the guest emits because it was told the
        // features exist.
        const auto stripped = strip_clip_and_cull_distance(std::span{info.pCode, code_size / sizeof(uint32_t)},
                                                           dev->second.strip_clip_distance, dev->second.strip_cull_distance);
        if (stripped)
        {
            info.codeSize = stripped->size() * sizeof(uint32_t);
            info.pCode = stripped->data();
        }

        VkShaderModule module{};
        const VkResult result = dev->second.create_shader_module(dev->second.handle, &info, nullptr, &module);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        const auto* code_bytes = static_cast<const std::byte*>(code);
        this->impl_->shader_modules.emplace(
            id, impl::shader_module_data{.handle = module,
                                         .device_id = device,
                                         .code = std::make_shared<const impl::shader_code>(
                                             impl::shader_code{.flags = flags, .code = {code_bytes, code_bytes + code_size}})});
        out_module = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_shader_module(uint64_t device, uint64_t shader_module)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->shader_modules.find(shader_module);
        if (it == this->impl_->shader_modules.end() || it->second.device_id != device)
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_shader_module)
        {
            dev->second.destroy_shader_module(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->shader_modules.erase(it);
    }

    int32_t vulkan_host::get_shader_module_identifier(uint64_t device, uint64_t shader_module, std::span<uint8_t> identifier,
                                                      uint32_t& identifier_size)
    {
        identifier_size = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto module = this->impl_->shader_modules.find(shader_module);
        if (dev == this->impl_->devices.end() || module == this->impl_->shader_modules.end() || module->second.device_id != device ||
            !dev->second.get_shader_module_identifier)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModuleIdentifierEXT result{};
        result.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        dev->second.get_shader_module_identifier(dev->second.handle, module->second.handle, &result);
        identifier_size = std::min<uint32_t>(result.identifierSize, static_cast<uint32_t>(identifier.size()));
        std::memcpy(identifier.data(), result.identifier, identifier_size);
        this->impl_->identified_code[std::string(reinterpret_cast<const char*>(identifier.data()), identifier_size)] = module->second.code;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_shader_module_create_info_identifier(uint64_t device, uint32_t flags, const void* code, size_t code_size,
                                                                  std::span<uint8_t> identifier, uint32_t& identifier_size)
    {
        identifier_size = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.get_shader_module_create_info_identifier)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.flags = static_cast<VkShaderModuleCreateFlags>(flags);
        create_info.codeSize = code_size;
        create_info.pCode = static_cast<const uint32_t*>(code);

        // The identifier has to match the module create_shader_module builds from the same code.
        const auto stripped = strip_clip_and_cull_distance(std::span{create_info.pCode, code_size / sizeof(uint32_t)},
                                                           dev->second.strip_clip_distance, dev->second.strip_cull_distance);
        if (stripped)
        {
            create_info.codeSize = stripped->size() * sizeof(uint32_t);
            create_info.pCode = stripped->data();
        }

        VkShaderModuleIdentifierEXT result{};
        result.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
        dev->second.get_shader_module_create_info_identifier(dev->second.handle, &create_info, &result);
        identifier_size = std::min<uint32_t>(result.identifierSize, static_cast<uint32_t>(identifier.size()));
        std::memcpy(identifier.data(), result.identifier, identifier_size);

        const auto* code_bytes = static_cast<const std::byte*>(code);
        this->impl_->identified_code[std::string(reinterpret_cast<const char*>(identifier.data()), identifier_size)] =
            std::make_shared<const impl::shader_code>(impl::shader_code{.flags = flags, .code = {code_bytes, code_bytes + code_size}});
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_image_view(uint64_t device, uint64_t image, uint32_t format, uint32_t aspect_mask, uint32_t view_type,
                                           uint32_t base_mip_level, uint32_t level_count, uint32_t base_array_layer, uint32_t layer_count,
                                           uint32_t swizzle_r, uint32_t swizzle_g, uint32_t swizzle_b, uint32_t swizzle_a,
                                           uint64_t& out_view)
    {
        out_view = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto img = this->impl_->images.find(image);
        if (dev == this->impl_->devices.end() || img == this->impl_->images.end() || img->second.device_id != device ||
            !dev->second.create_image_view)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->image_views.emplace(id, impl::image_view_data{.handle = view,
                                                                   .device_id = device,
                                                                   .create = {.image = image,
                                                                              .format = format,
                                                                              .aspect_mask = aspect_mask,
                                                                              .view_type = view_type,
                                                                              .base_mip_level = base_mip_level,
                                                                              .level_count = level_count,
                                                                              .base_array_layer = base_array_layer,
                                                                              .layer_count = layer_count,
                                                                              .swizzle = {swizzle_r, swizzle_g, swizzle_b, swizzle_a}}});
        out_view = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_image_view(uint64_t device, uint64_t image_view)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->image_views.find(image_view);
        if (it == this->impl_->image_views.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || buf == this->impl_->buffers.end() || buf->second.device_id != device ||
            !dev->second.create_buffer_view)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->buffer_views.emplace(
            id, impl::buffer_view_data{
                    .handle = view, .device_id = device, .create = {.buffer = buffer, .format = format, .offset = offset, .range = range}});
        out_view = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_buffer_view(uint64_t device, uint64_t buffer_view)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->buffer_views.find(buffer_view);
        if (it == this->impl_->buffer_views.end() || it->second.device_id != device)
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
        if (cb == this->impl_->command_buffers.end() || src == this->impl_->buffers.end() || dst == this->impl_->buffers.end() ||
            src->second.device_id != cb->second.device_id || dst->second.device_id != cb->second.device_id)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->query_pools.emplace(id, impl::query_pool_data{.handle = pool,
                                                                   .device_id = device,
                                                                   .query_type = query_type,
                                                                   .pipeline_statistics = pipeline_statistics,
                                                                   .query_count = query_count});
        out_pool = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_query_pool(uint64_t device, uint64_t query_pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->query_pools.find(query_pool);
        if (it == this->impl_->query_pools.end() || it->second.device_id != device)
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
        if (dev == this->impl_->devices.end() || qp == this->impl_->query_pools.end() || qp->second.device_id != device ||
            !dev->second.reset_query_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.reset_query_pool(dev->second.handle, qp->second.handle, first_query, query_count);
        qp->second.forget_restored(first_query, query_count);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_query_pool_results(uint64_t device, uint64_t query_pool, uint32_t first_query, uint32_t query_count,
                                                uint32_t flags, void* out, size_t out_size, size_t stride, size_t& out_written)
    {
        out_written = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (dev == this->impl_->devices.end() || qp == this->impl_->query_pools.end() || qp->second.device_id != device ||
            !dev->second.get_query_pool_results)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // vkGetQueryPoolResults writes one result element per query at out + i*stride. The element size is
        // driver-determined by the query type and flags, and can exceed a guest-chosen (small) stride. Bound
        // the total extent against out_size so the driver cannot write past the caller's buffer, regardless
        // of the guest-supplied stride (including stride == 0).
        const uint64_t value_size = (flags & VK_QUERY_RESULT_64_BIT) ? 8u : 4u;
        uint64_t value_count = 1;
        if (qp->second.query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS)
        {
            value_count = static_cast<uint64_t>(std::popcount(qp->second.pipeline_statistics));
        }
        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
        {
            value_count += 1;
        }
        const uint64_t element_size = value_count * value_size;
        const uint64_t effective_stride = std::max<uint64_t>(stride, element_size);
        if (query_count > 0 && (static_cast<uint64_t>(query_count - 1) * effective_stride + element_size) > out_size)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (!qp->second.restored_results.empty())
        {
            // Restored queries are not available in the re-created pool, so they are answered one by one.
            VkResult result = VK_SUCCESS;
            for (uint32_t i = 0; i < query_count; ++i)
            {
                auto* element = static_cast<std::byte*>(out) + (i * effective_stride);
                if (qp->second.restored_results.contains(first_query + i))
                {
                    const auto restored = qp->second.restored_result(first_query + i, flags);
                    std::memcpy(element, restored.data(), restored.size());
                    continue;
                }

                const VkResult single =
                    dev->second.get_query_pool_results(dev->second.handle, qp->second.handle, first_query + i, 1, element_size, element,
                                                       element_size, static_cast<VkQueryResultFlags>(flags));
                if (single != VK_SUCCESS && result == VK_SUCCESS)
                {
                    result = single;
                }
            }

            if (result == VK_SUCCESS)
            {
                out_written = out_size;
            }
            return result;
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
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_reset_query_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_reset_query_pool(cb->second.handle, qp->second.handle, first_query, query_count);
        cb->second.query_resets.emplace_back(query_pool, first_query, query_count);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_begin_query(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
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
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_begin_query_indexed(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t flags,
                                                 uint32_t index)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_begin_query_indexed)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        dev->second.cmd_begin_query_indexed(cb->second.handle, qp->second.handle, query, static_cast<VkQueryControlFlags>(flags), index);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_end_query_indexed(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t index)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_end_query_indexed)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        dev->second.cmd_end_query_indexed(cb->second.handle, qp->second.handle, query, index);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_transform_feedback_buffers(uint64_t command_buffer, uint32_t first_binding,
                                                             std::span<const uint64_t> buffer_ids, std::span<const uint64_t> offsets,
                                                             std::span<const uint64_t> sizes)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end() || buffer_ids.empty() || buffer_ids.size() != offsets.size() ||
            buffer_ids.size() != sizes.size())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_transform_feedback_buffers)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        std::vector<VkBuffer> buffers;
        buffers.reserve(buffer_ids.size());
        for (size_t i = 0; i < buffer_ids.size(); ++i)
        {
            const auto buffer = this->impl_->buffers.find(buffer_ids[i]);
            if (buffer == this->impl_->buffers.end() || buffer->second.device_id != cb->second.device_id ||
                offsets[i] >= buffer->second.size || (sizes[i] != VK_WHOLE_SIZE && sizes[i] > buffer->second.size - offsets[i]))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            buffers.push_back(buffer->second.handle);
        }
        dev->second.cmd_bind_transform_feedback_buffers(cb->second.handle, first_binding, static_cast<uint32_t>(buffers.size()),
                                                        buffers.data(), offsets.data(), sizes.data());
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_transform_feedback(uint64_t command_buffer, uint32_t first_counter_buffer,
                                                std::span<const uint64_t> counter_buffer_ids,
                                                std::span<const uint64_t> counter_buffer_offsets, bool has_counter_buffers,
                                                bool has_counter_buffer_offsets, bool begin)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end() || counter_buffer_ids.size() != counter_buffer_offsets.size() ||
            (!has_counter_buffers && has_counter_buffer_offsets))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() ||
            (begin ? !dev->second.cmd_begin_transform_feedback : !dev->second.cmd_end_transform_feedback))
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        std::vector<VkBuffer> counter_buffers;
        if (has_counter_buffers)
        {
            counter_buffers.reserve(counter_buffer_ids.size());
            for (size_t i = 0; i < counter_buffer_ids.size(); ++i)
            {
                if (counter_buffer_ids[i] == gpu_bridge::null_object)
                {
                    counter_buffers.push_back(VK_NULL_HANDLE);
                    continue;
                }

                const auto buffer = this->impl_->buffers.find(counter_buffer_ids[i]);
                const uint64_t offset = has_counter_buffer_offsets ? counter_buffer_offsets[i] : 0;
                if (buffer == this->impl_->buffers.end() || buffer->second.device_id != cb->second.device_id ||
                    offset > buffer->second.size || buffer->second.size - offset < sizeof(uint32_t))
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                counter_buffers.push_back(buffer->second.handle);
            }
        }

        const bool has_entries = !counter_buffer_ids.empty();
        const VkBuffer* buffers = has_counter_buffers && has_entries ? counter_buffers.data() : nullptr;
        const VkDeviceSize* offsets =
            has_counter_buffers && has_counter_buffer_offsets && has_entries ? counter_buffer_offsets.data() : nullptr;
        if (begin)
        {
            dev->second.cmd_begin_transform_feedback(cb->second.handle, first_counter_buffer,
                                                     static_cast<uint32_t>(counter_buffer_ids.size()), buffers, offsets);
        }
        else
        {
            dev->second.cmd_end_transform_feedback(cb->second.handle, first_counter_buffer,
                                                   static_cast<uint32_t>(counter_buffer_ids.size()), buffers, offsets);
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_begin_transform_feedback(uint64_t command_buffer, uint32_t first_counter_buffer,
                                                      std::span<const uint64_t> counter_buffers,
                                                      std::span<const uint64_t> counter_buffer_offsets, bool has_counter_buffers,
                                                      bool has_counter_buffer_offsets)
    {
        return this->cmd_transform_feedback(command_buffer, first_counter_buffer, counter_buffers, counter_buffer_offsets,
                                            has_counter_buffers, has_counter_buffer_offsets, true);
    }

    int32_t vulkan_host::cmd_end_transform_feedback(uint64_t command_buffer, uint32_t first_counter_buffer,
                                                    std::span<const uint64_t> counter_buffers,
                                                    std::span<const uint64_t> counter_buffer_offsets, bool has_counter_buffers,
                                                    bool has_counter_buffer_offsets)
    {
        return this->cmd_transform_feedback(command_buffer, first_counter_buffer, counter_buffers, counter_buffer_offsets,
                                            has_counter_buffers, has_counter_buffer_offsets, false);
    }

    int32_t vulkan_host::cmd_draw_indirect_byte_count(uint64_t command_buffer, uint32_t instance_count, uint32_t first_instance,
                                                      uint64_t counter_buffer, uint64_t counter_buffer_offset, uint32_t counter_offset,
                                                      uint32_t vertex_stride)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buffer = this->impl_->buffers.find(counter_buffer);
        if (cb == this->impl_->command_buffers.end() || buffer == this->impl_->buffers.end() ||
            buffer->second.device_id != cb->second.device_id || counter_buffer_offset > buffer->second.size ||
            buffer->second.size - counter_buffer_offset < sizeof(uint32_t) || vertex_stride == 0)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indirect_byte_count)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        dev->second.cmd_draw_indirect_byte_count(cb->second.handle, instance_count, first_instance, buffer->second.handle,
                                                 counter_buffer_offset, counter_offset, vertex_stride);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_write_timestamp(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t pipeline_stage)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_write_timestamp2(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint64_t pipeline_stage)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() ||
            qp->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!dev->second.cmd_write_timestamp2)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        dev->second.cmd_write_timestamp2(cb->second.handle, static_cast<VkPipelineStageFlags2>(pipeline_stage), qp->second.handle, query);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_copy_query_pool_results(uint64_t command_buffer, uint64_t query_pool, uint32_t first_query,
                                                     uint32_t query_count, uint64_t destination_buffer, uint64_t destination_offset,
                                                     uint64_t stride, uint32_t flags)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto qp = this->impl_->query_pools.find(query_pool);
        const auto buffer = this->impl_->buffers.find(destination_buffer);
        if (cb == this->impl_->command_buffers.end() || qp == this->impl_->query_pools.end() || buffer == this->impl_->buffers.end() ||
            qp->second.device_id != cb->second.device_id || buffer->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_copy_query_pool_results)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        if (qp->second.restored_results.empty())
        {
            dev->second.cmd_copy_query_pool_results(cb->second.handle, qp->second.handle, first_query, query_count, buffer->second.handle,
                                                    destination_offset, stride, static_cast<VkQueryResultFlags>(flags));
            return VK_SUCCESS;
        }

        // Restored queries are written into the buffer directly, since the re-created pool does not hold them.
        for (uint32_t i = 0; i < query_count; ++i)
        {
            const auto offset = destination_offset + (i * stride);
            if (!qp->second.restored_results.contains(first_query + i))
            {
                dev->second.cmd_copy_query_pool_results(cb->second.handle, qp->second.handle, first_query + i, 1, buffer->second.handle,
                                                        offset, stride, static_cast<VkQueryResultFlags>(flags));
                continue;
            }

            const auto restored = qp->second.restored_result(first_query + i, flags);
            dev->second.cmd_update_buffer(cb->second.handle, buffer->second.handle, offset, restored.size(), restored.data());
        }
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

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->render_passes.emplace(id, impl::render_pass_data{.handle = render_pass,
                                                                      .device_id = device,
                                                                      .has_depth = has_depth,
                                                                      .create = {.format = format,
                                                                                 .load_op = load_op,
                                                                                 .store_op = store_op,
                                                                                 .initial_layout = initial_layout,
                                                                                 .final_layout = final_layout,
                                                                                 .depth_format = depth_format}});
        out_render_pass = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_render_pass(uint64_t device, uint64_t render_pass)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->render_passes.find(render_pass);
        if (it == this->impl_->render_passes.end() || it->second.device_id != device)
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
            rp->second.device_id != device || view->second.device_id != device || !dev->second.create_framebuffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::array<VkImageView, 2> views{view->second.handle, VK_NULL_HANDLE};
        uint32_t attachment_count = 1;
        if (depth_view != 0)
        {
            const auto dview = this->impl_->image_views.find(depth_view);
            if (dview == this->impl_->image_views.end() || dview->second.device_id != device)
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

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->framebuffers.emplace(id, impl::framebuffer_data{.handle = framebuffer,
                                                                     .device_id = device,
                                                                     .create = {.render_pass = render_pass,
                                                                                .render_pass_create = rp->second.create,
                                                                                .image_view = image_view,
                                                                                .depth_view = depth_view,
                                                                                .width = width,
                                                                                .height = height}});
        out_framebuffer = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_framebuffer(uint64_t device, uint64_t framebuffer)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->framebuffers.find(framebuffer);
        if (it == this->impl_->framebuffers.end() || it->second.device_id != device)
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
            if (sl == this->impl_->descriptor_set_layouts.end() || sl->second.device_id != device)
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

        const uint64_t id = this->impl_->allocate_id();
        auto create = std::make_shared<impl::pipeline_layout_params>();
        create->push_constant_stages = push_constant_stages;
        create->push_constant_size = push_constant_size;
        create->set_layouts.assign(set_layouts.begin(), set_layouts.end());
        for (const uint64_t set_layout : set_layouts)
        {
            create->set_layout_creates.push_back(this->impl_->descriptor_set_layouts.at(set_layout).create);
        }

        this->impl_->pipeline_layouts.emplace(
            id, impl::pipeline_layout_data{.handle = layout, .device_id = device, .create = std::move(create)});
        out_layout = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline_layout(uint64_t device, uint64_t pipeline_layout)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (it == this->impl_->pipeline_layouts.end() || it->second.device_id != device)
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_pipeline_layout)
        {
            dev->second.destroy_pipeline_layout(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->pipeline_layouts.erase(it);
    }

    int32_t vulkan_host::create_descriptor_set_layout(uint64_t device, uint32_t flags, std::span<const descriptor_binding> bindings,
                                                      uint64_t& out_layout)
    {
        out_layout = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_descriptor_set_layout)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
        std::vector<VkDescriptorBindingFlags> vk_binding_flags;
        vk_bindings.reserve(bindings.size());
        vk_binding_flags.reserve(bindings.size());
        for (const descriptor_binding& b : bindings)
        {
            VkDescriptorSetLayoutBinding vb{};
            vb.binding = b.binding;
            vb.descriptorType = static_cast<VkDescriptorType>(b.descriptor_type);
            vb.descriptorCount = b.descriptor_count;
            vb.stageFlags = b.stage_flags;
            vk_bindings.push_back(vb);
            vk_binding_flags.push_back(b.binding_flags);
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{};
        binding_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags.bindingCount = static_cast<uint32_t>(vk_binding_flags.size());
        binding_flags.pBindingFlags = vk_binding_flags.data();

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.pNext = vk_binding_flags.empty() ? nullptr : &binding_flags;
        info.flags = flags;
        info.bindingCount = static_cast<uint32_t>(vk_bindings.size());
        info.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();

        VkDescriptorSetLayout layout{};
        const VkResult result = dev->second.create_descriptor_set_layout(dev->second.handle, &info, nullptr, &layout);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->descriptor_set_layouts.emplace(
            id, impl::descriptor_set_layout_data{.handle = layout,
                                                 .device_id = device,
                                                 .create = std::make_shared<const impl::set_layout_params>(impl::set_layout_params{
                                                     .flags = flags, .bindings = {bindings.begin(), bindings.end()}})});
        out_layout = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_descriptor_set_layout_support(uint64_t device, uint32_t flags, std::span<const descriptor_binding> bindings,
                                                           uint32_t& supported, uint32_t& max_variable_descriptor_count)
    {
        supported = VK_FALSE;
        max_variable_descriptor_count = 0;

        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.get_descriptor_set_layout_support)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
        std::vector<VkDescriptorBindingFlags> vk_binding_flags;
        vk_bindings.reserve(bindings.size());
        vk_binding_flags.reserve(bindings.size());
        for (const descriptor_binding& b : bindings)
        {
            VkDescriptorSetLayoutBinding vb{};
            vb.binding = b.binding;
            vb.descriptorType = static_cast<VkDescriptorType>(b.descriptor_type);
            vb.descriptorCount = b.descriptor_count;
            vb.stageFlags = b.stage_flags;
            vk_bindings.push_back(vb);
            vk_binding_flags.push_back(b.binding_flags);
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{};
        binding_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags.bindingCount = static_cast<uint32_t>(vk_binding_flags.size());
        binding_flags.pBindingFlags = vk_binding_flags.data();

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.pNext = vk_binding_flags.empty() ? nullptr : &binding_flags;
        info.flags = flags;
        info.bindingCount = static_cast<uint32_t>(vk_bindings.size());
        info.pBindings = vk_bindings.empty() ? nullptr : vk_bindings.data();

        VkDescriptorSetLayoutSupport support{};
        support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
        VkDescriptorSetVariableDescriptorCountLayoutSupport variable_support{};
        variable_support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT;
        support.pNext = &variable_support;
        dev->second.get_descriptor_set_layout_support(dev->second.handle, &info, &support);
        supported = support.supported;
        max_variable_descriptor_count = variable_support.maxVariableDescriptorCount;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_descriptor_set_layout(uint64_t device, uint64_t layout)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->descriptor_set_layouts.find(layout);
        if (it == this->impl_->descriptor_set_layouts.end() || it->second.device_id != device)
        {
            return;
        }
        if (dev != this->impl_->devices.end() && it->second.handle && dev->second.destroy_descriptor_set_layout)
        {
            dev->second.destroy_descriptor_set_layout(dev->second.handle, it->second.handle, nullptr);
        }
        this->impl_->descriptor_set_layouts.erase(it);
    }

    int32_t vulkan_host::create_descriptor_pool(uint64_t device, uint32_t max_sets, uint32_t flags,
                                                uint32_t max_inline_uniform_block_bindings, std::span<const descriptor_pool_size> sizes,
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
        info.flags = flags;
        info.maxSets = max_sets;
        info.poolSizeCount = static_cast<uint32_t>(vk_sizes.size());
        info.pPoolSizes = vk_sizes.empty() ? nullptr : vk_sizes.data();

        VkDescriptorPoolInlineUniformBlockCreateInfo inline_uniform_blocks{};
        if (max_inline_uniform_block_bindings != 0)
        {
            inline_uniform_blocks.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO;
            inline_uniform_blocks.maxInlineUniformBlockBindings = max_inline_uniform_block_bindings;
            info.pNext = &inline_uniform_blocks;
        }

        VkDescriptorPool pool{};
        const VkResult result = dev->second.create_descriptor_pool(dev->second.handle, &info, nullptr, &pool);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->descriptor_pools.emplace(
            id, impl::descriptor_pool_data{.handle = pool,
                                           .device_id = device,
                                           .create = {.max_sets = max_sets,
                                                      .flags = flags,
                                                      .max_inline_uniform_block_bindings = max_inline_uniform_block_bindings,
                                                      .sizes = {sizes.begin(), sizes.end()}}});
        out_pool = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::reset_descriptor_pool(uint64_t device, uint64_t pool, uint32_t flags)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->descriptor_pools.find(pool);
        if (dev == this->impl_->devices.end() || it == this->impl_->descriptor_pools.end() || it->second.device_id != device ||
            !dev->second.reset_descriptor_pool)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const int32_t result =
            dev->second.reset_descriptor_pool(dev->second.handle, it->second.handle, static_cast<VkDescriptorPoolResetFlags>(flags));
        if (result == VK_SUCCESS)
        {
            // The reset implicitly frees every set allocated from this pool; drop their ids.
            this->impl_->erase_owned(this->impl_->descriptor_sets, [&](const impl::descriptor_set_data& d) { return d.pool_id == pool; });
        }
        return result;
    }

    void vulkan_host::destroy_descriptor_pool(uint64_t device, uint64_t pool)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->descriptor_pools.find(pool);
        if (it == this->impl_->descriptor_pools.end() || it->second.device_id != device)
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
                                                  std::span<const uint32_t> variable_descriptor_counts, std::span<uint64_t> out_sets,
                                                  uint32_t& out_count)
    {
        out_count = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->descriptor_pools.find(pool);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->descriptor_pools.end() || pool_it->second.device_id != device ||
            !dev->second.allocate_descriptor_sets ||
            (!variable_descriptor_counts.empty() && variable_descriptor_counts.size() != set_layouts.size()))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSetLayout> vk_layouts;
        vk_layouts.reserve(set_layouts.size());
        for (const uint64_t id : set_layouts)
        {
            const auto sl = this->impl_->descriptor_set_layouts.find(id);
            if (sl == this->impl_->descriptor_set_layouts.end() || sl->second.device_id != device)
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

        VkDescriptorSetVariableDescriptorCountAllocateInfo variable_counts{};
        if (!variable_descriptor_counts.empty())
        {
            variable_counts.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
            variable_counts.descriptorSetCount = static_cast<uint32_t>(variable_descriptor_counts.size());
            variable_counts.pDescriptorCounts = variable_descriptor_counts.data();
            info.pNext = &variable_counts;
        }

        std::vector<VkDescriptorSet> sets(vk_layouts.size());
        const VkResult result = dev->second.allocate_descriptor_sets(dev->second.handle, &info, sets.data());
        if (result != VK_SUCCESS)
        {
            return result;
        }

        out_count = static_cast<uint32_t>(sets.size());
        for (size_t i = 0; i < sets.size(); ++i)
        {
            const uint64_t id = this->impl_->allocate_id();
            impl::descriptor_set_data data{.handle = sets[i], .device_id = device, .pool_id = pool, .buffer_bindings = {}};
            data.layout_id = set_layouts[i];
            data.layout_create = this->impl_->descriptor_set_layouts.at(set_layouts[i]).create;
            if (!variable_descriptor_counts.empty())
            {
                data.variable_descriptor_count = variable_descriptor_counts[i];
            }
            this->impl_->descriptor_sets.emplace(id, std::move(data));
            if (i < out_sets.size())
            {
                out_sets[i] = id;
            }
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::free_descriptor_sets(uint64_t device, uint64_t pool, std::span<const uint64_t> sets)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto pool_it = this->impl_->descriptor_pools.find(pool);
        if (dev == this->impl_->devices.end() || pool_it == this->impl_->descriptor_pools.end() || pool_it->second.device_id != device ||
            !dev->second.free_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkDescriptorSet> handles;
        handles.reserve(sets.size());
        for (const uint64_t id : sets)
        {
            const auto set = this->impl_->descriptor_sets.find(id);
            if (set == this->impl_->descriptor_sets.end() || set->second.device_id != device || set->second.pool_id != pool)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            handles.push_back(set->second.handle);
        }

        const VkResult result = dev->second.free_descriptor_sets(dev->second.handle, pool_it->second.handle,
                                                                 static_cast<uint32_t>(handles.size()), handles.data());
        if (result == VK_SUCCESS)
        {
            for (const uint64_t id : sets)
            {
                this->impl_->descriptor_sets.erase(id);
            }
        }
        return result;
    }

    int32_t vulkan_host::update_descriptor_sets(uint64_t device, std::span<const descriptor_write> writes,
                                                std::span<const descriptor_copy> copies)
    {
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.update_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // Buffer infos, image infos and inline-uniform blocks are kept in stable vectors so the
        // VkWriteDescriptorSet pointers remain valid until the driver call below. This runs ~100k times/s
        // in heavy scenes, so the buffers are reused (single emulator thread, no reentrancy) instead of
        // allocated per call.
        static thread_local std::vector<VkWriteDescriptorSet> vk_writes;
        static thread_local std::vector<VkDescriptorBufferInfo> buffer_infos;
        static thread_local std::vector<VkDescriptorImageInfo> image_infos;
        static thread_local std::vector<VkBufferView> texel_buffer_views;
        static thread_local std::vector<VkWriteDescriptorSetInlineUniformBlock> inline_uniform_blocks;
        static thread_local std::vector<VkCopyDescriptorSet> vk_copies;
        vk_writes.clear();
        buffer_infos.resize(writes.size());
        image_infos.resize(writes.size());
        texel_buffer_views.resize(writes.size());
        inline_uniform_blocks.resize(writes.size());
        vk_writes.reserve(writes.size());
        vk_copies.clear();
        vk_copies.reserve(copies.size());

        // Consecutive writes usually target the same descriptor set; cache the last lookup to avoid a hash
        // probe per write.
        uint64_t cached_set_id = 0;
        VkDescriptorSet cached_set_handle = VK_NULL_HANDLE;
        impl::descriptor_set_data* cached_set = nullptr;

        for (size_t i = 0; i < writes.size(); ++i)
        {
            const descriptor_write& w = writes[i];
            if (w.dst_set != cached_set_id || cached_set == nullptr)
            {
                const auto set = this->impl_->descriptor_sets.find(w.dst_set);
                if (set == this->impl_->descriptor_sets.end() || set->second.device_id != device)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                cached_set_id = w.dst_set;
                cached_set_handle = set->second.handle;
                cached_set = &set->second;
            }

            VkWriteDescriptorSet vw{};
            vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            vw.dstSet = cached_set_handle;
            vw.dstBinding = w.dst_binding;
            vw.dstArrayElement = w.dst_array_element;
            vw.descriptorCount = 1;
            vw.descriptorType = static_cast<VkDescriptorType>(w.descriptor_type);

            const bool is_image =
                (w.descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || w.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                 w.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || w.descriptor_type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ||
                 w.descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER);
            const bool is_texel_buffer = (w.descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                                          w.descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
            if (w.descriptor_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
            {
                VkWriteDescriptorSetInlineUniformBlock& inline_uniform_block = inline_uniform_blocks[i];
                inline_uniform_block = {};
                inline_uniform_block.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
                inline_uniform_block.dataSize = static_cast<uint32_t>(w.inline_uniform_data.size());
                inline_uniform_block.pData = w.inline_uniform_data.empty() ? nullptr : w.inline_uniform_data.data();
                vw.pNext = &inline_uniform_block;
                vw.descriptorCount = inline_uniform_block.dataSize;
            }
            else if (is_texel_buffer)
            {
                VkBufferView& buffer_view = texel_buffer_views[i];
                buffer_view = VK_NULL_HANDLE;
                if (w.buffer_or_view != 0)
                {
                    const auto view = this->impl_->buffer_views.find(w.buffer_or_view);
                    if (view == this->impl_->buffer_views.end() || view->second.device_id != device)
                    {
                        return VK_ERROR_INITIALIZATION_FAILED;
                    }
                    buffer_view = view->second.handle;
                }
                vw.pTexelBufferView = &buffer_view;
            }
            else if (is_image)
            {
                const auto view = this->impl_->image_views.find(w.image_view);
                if (view != this->impl_->image_views.end() && view->second.device_id != device)
                {
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
                VkDescriptorImageInfo& ii = image_infos[i];
                ii.sampler = VK_NULL_HANDLE;
                if (w.sampler != 0)
                {
                    const auto smp = this->impl_->samplers.find(w.sampler);
                    if (smp == this->impl_->samplers.end() || smp->second.device_id != device)
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
                if (w.buffer_or_view == 0)
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
                    const auto buf = this->impl_->buffers.find(w.buffer_or_view);
                    if (buf == this->impl_->buffers.end() || buf->second.device_id != device)
                    {
                        return VK_ERROR_INITIALIZATION_FAILED;
                    }
                    bi.buffer = buf->second.handle;
                    bi.offset = w.offset;
                    bi.range = w.range;
                    cached_set->buffer_bindings[w.dst_binding] = impl::bound_buffer_info{
                        .buffer_id = w.buffer_or_view, .offset = w.offset, .range = w.range, .type = w.descriptor_type};
                }
                vw.pBufferInfo = &bi;
            }
            vk_writes.push_back(vw);
        }

        for (const descriptor_copy& c : copies)
        {
            const auto src_set = this->impl_->descriptor_sets.find(c.src_set);
            const auto dst_set = this->impl_->descriptor_sets.find(c.dst_set);
            if (src_set == this->impl_->descriptor_sets.end() || dst_set == this->impl_->descriptor_sets.end() ||
                src_set->second.device_id != device || dst_set->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            VkCopyDescriptorSet copy{};
            copy.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET;
            copy.srcSet = src_set->second.handle;
            copy.srcBinding = c.src_binding;
            copy.srcArrayElement = c.src_array_element;
            copy.dstSet = dst_set->second.handle;
            copy.dstBinding = c.dst_binding;
            copy.dstArrayElement = c.dst_array_element;
            copy.descriptorCount = c.descriptor_count;
            vk_copies.push_back(copy);
        }

        if (!vk_writes.empty() || !vk_copies.empty())
        {
            dev->second.update_descriptor_sets(dev->second.handle, static_cast<uint32_t>(vk_writes.size()), vk_writes.data(),
                                               static_cast<uint32_t>(vk_copies.size()), vk_copies.data());
        }

        cached_set_id = 0;
        cached_set = nullptr;
        for (const descriptor_write& w : writes)
        {
            if (w.dst_set != cached_set_id || cached_set == nullptr)
            {
                cached_set_id = w.dst_set;
                cached_set = &this->impl_->descriptor_sets.at(w.dst_set);
            }

            if (w.descriptor_type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK)
            {
                auto& block = cached_set->inline_blocks[w.dst_binding];
                block.resize(std::max<size_t>(block.size(), w.dst_array_element + w.inline_uniform_data.size()));
                std::ranges::copy(w.inline_uniform_data, block.begin() + w.dst_array_element);
                continue;
            }

            cached_set->slots[impl::descriptor_slot_key(w.dst_binding, w.dst_array_element)] = impl::descriptor_slot{
                .descriptor_type = w.descriptor_type,
                .buffer_or_view = w.buffer_or_view,
                .offset = w.offset,
                .range = w.range,
                .sampler = w.sampler,
                .image_view = w.image_view,
                .image_layout = w.image_layout,
            };
        }

        for (const descriptor_copy& c : copies)
        {
            const auto& source = this->impl_->descriptor_sets.at(c.src_set);
            auto& destination = this->impl_->descriptor_sets.at(c.dst_set);
            const auto from = impl::consecutive_descriptors(source, c.src_binding, c.src_array_element, c.descriptor_count);
            const auto to = impl::consecutive_descriptors(destination, c.dst_binding, c.dst_array_element, c.descriptor_count);
            const auto count = std::min(from.size(), to.size());

            // Inline uniform blocks count bytes instead of descriptors.
            if (impl::is_inline_block(source, c.src_binding))
            {
                std::vector<std::byte> bytes(count);
                for (size_t i = 0; i < count; ++i)
                {
                    const auto block = source.inline_blocks.find(from[i].first);
                    bytes[i] = block != source.inline_blocks.end() && from[i].second < block->second.size() ? block->second[from[i].second]
                                                                                                            : std::byte{};
                }

                for (size_t i = 0; i < count; ++i)
                {
                    auto& block = destination.inline_blocks[to[i].first];
                    block.resize(std::max<size_t>(block.size(), static_cast<size_t>(to[i].second) + 1));
                    block[to[i].second] = bytes[i];
                }
                continue;
            }

            std::vector<std::optional<impl::descriptor_slot>> slots(count);
            for (size_t i = 0; i < count; ++i)
            {
                if (const auto slot = source.slots.find(impl::descriptor_slot_key(from[i].first, from[i].second));
                    slot != source.slots.end())
                {
                    slots[i] = slot->second;
                }
            }

            for (size_t i = 0; i < count; ++i)
            {
                const auto key = impl::descriptor_slot_key(to[i].first, to[i].second);
                if (slots[i])
                {
                    destination.slots[key] = *slots[i];
                }
                else
                {
                    destination.slots.erase(key);
                }
            }
        }

        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_pipeline_cache(uint64_t device, uint32_t flags, std::span<const uint8_t> initial_data,
                                               uint64_t& out_pipeline_cache)
    {
        out_pipeline_cache = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || !dev->second.create_pipeline_cache)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPipelineCacheCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        info.flags = static_cast<VkPipelineCacheCreateFlags>(flags);
        info.initialDataSize = initial_data.size();
        info.pInitialData = initial_data.empty() ? nullptr : initial_data.data();

        VkPipelineCache cache = VK_NULL_HANDLE;
        const VkResult result = dev->second.create_pipeline_cache(dev->second.handle, &info, nullptr, &cache);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        this->impl_->pipeline_caches.emplace(id, impl::pipeline_cache_data{.handle = cache, .device_id = device, .flags = flags});
        out_pipeline_cache = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline_cache(uint64_t device, uint64_t pipeline_cache)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto cache = this->impl_->pipeline_caches.find(pipeline_cache);
        if (cache == this->impl_->pipeline_caches.end() || cache->second.device_id != device)
        {
            return;
        }

        if (dev != this->impl_->devices.end() && cache->second.handle && dev->second.destroy_pipeline_cache)
        {
            dev->second.destroy_pipeline_cache(dev->second.handle, cache->second.handle, nullptr);
        }
        this->impl_->pipeline_caches.erase(cache);
    }

    int32_t vulkan_host::get_pipeline_cache_data(uint64_t device, uint64_t pipeline_cache, void* out, size_t out_size, bool has_data,
                                                 size_t& data_size)
    {
        data_size = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto cache = this->impl_->pipeline_caches.find(pipeline_cache);
        if (dev == this->impl_->devices.end() || cache == this->impl_->pipeline_caches.end() || cache->second.device_id != device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!dev->second.get_pipeline_cache_data)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        std::byte dummy{};
        void* data = nullptr;
        if (has_data)
        {
            data = out;
            if (!data)
            {
                data = &dummy;
            }
        }
        size_t size = has_data ? out_size : 0;
        const VkResult result = dev->second.get_pipeline_cache_data(dev->second.handle, cache->second.handle, &size, data);
        data_size = size;
        return result;
    }

    int32_t vulkan_host::merge_pipeline_caches(uint64_t device, uint64_t destination_cache, std::span<const uint64_t> source_caches)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto destination = this->impl_->pipeline_caches.find(destination_cache);
        if (dev == this->impl_->devices.end() || destination == this->impl_->pipeline_caches.end() ||
            destination->second.device_id != device)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (!dev->second.merge_pipeline_caches)
        {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        std::vector<VkPipelineCache> sources;
        sources.reserve(source_caches.size());
        for (const uint64_t source_id : source_caches)
        {
            const auto source = this->impl_->pipeline_caches.find(source_id);
            if (source == this->impl_->pipeline_caches.end() || source->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            sources.push_back(source->second.handle);
        }

        return dev->second.merge_pipeline_caches(dev->second.handle, destination->second.handle, static_cast<uint32_t>(sources.size()),
                                                 sources.empty() ? nullptr : sources.data());
    }

    int32_t vulkan_host::create_graphics_pipeline(uint64_t device, uint64_t pipeline_cache, uint64_t render_pass, uint64_t pipeline_layout,
                                                  const shader_stage_source& vertex_shader, const shader_stage_source& fragment_shader,
                                                  uint32_t flags, uint32_t width, uint32_t height, std::span<const vertex_binding> bindings,
                                                  std::span<const vertex_attribute> attributes, std::span<const vertex_divisor> divisors,
                                                  const depth_state& depth, std::span<const uint32_t> color_formats, uint32_t depth_format,
                                                  uint32_t stencil_format, uint32_t rasterization_samples, uint32_t primitive_topology,
                                                  uint32_t primitive_restart_enable, uint32_t rasterization_stream,
                                                  uint32_t rasterization_stream_flags, std::span<const uint32_t> dynamic_states,
                                                  const specialization& vs_spec, const specialization& fs_spec,
                                                  std::span<const color_blend_attachment> blend_attachments_in, uint64_t& out_pipeline)
    {
        out_pipeline = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (dev == this->impl_->devices.end() || layout == this->impl_->pipeline_layouts.end() || layout->second.device_id != device ||
            !dev->second.create_graphics_pipelines)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto resolve_stage = [&](const shader_stage_source& source, VkShaderModule& module) {
            const bool has_module = source.module != 0;
            const bool has_identifier = !source.identifier.empty();
            if (has_module == has_identifier || source.identifier.size() > VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT)
            {
                return false;
            }
            if (!has_module)
            {
                module = VK_NULL_HANDLE;
                return true;
            }
            const auto shader = this->impl_->shader_modules.find(source.module);
            if (shader == this->impl_->shader_modules.end() || shader->second.device_id != device)
            {
                return false;
            }
            module = shader->second.handle;
            return true;
        };

        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        if (!resolve_stage(vertex_shader, vertex_module) || !resolve_stage(fragment_shader, fragment_module))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkPipelineCache cache_handle = VK_NULL_HANDLE;
        if (pipeline_cache != 0)
        {
            const auto cache = this->impl_->pipeline_caches.find(pipeline_cache);
            if (cache == this->impl_->pipeline_caches.end() || cache->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            cache_handle = cache->second.handle;
        }

        // render_pass == 0 means the pipeline targets dynamic rendering (DXVK 2.x): there is no render-pass
        // object; the attachment formats come in via VkPipelineRenderingCreateInfo and viewport/scissor are
        // dynamic. Otherwise resolve the render pass and bake a static viewport as before.
        const bool dynamic_rendering = (render_pass == 0);
        VkRenderPass rp_handle = VK_NULL_HANDLE;
        if (!dynamic_rendering)
        {
            const auto rp = this->impl_->render_passes.find(render_pass);
            if (rp == this->impl_->render_passes.end() || rp->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            rp_handle = rp->second.handle;
        }

        // Rebuild a VkSpecializationInfo per stage from the forwarded entries/data. The map-entry vectors and
        // the two info structs must outlive the vkCreateGraphicsPipelines call below.
        std::array<std::vector<VkSpecializationMapEntry>, 2> spec_map_entries;
        std::array<VkSpecializationInfo, 2> spec_infos{};

        const auto valid_spec = [](const specialization& spec) {
            for (const auto& e : spec.entries)
            {
                if (e.offset > spec.data.size() || e.size > spec.data.size() - e.offset)
                {
                    return false;
                }
            }
            return true;
        };

        if (!valid_spec(vs_spec) || !valid_spec(fs_spec))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

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

        std::array<VkPipelineShaderStageModuleIdentifierCreateInfoEXT, 2> stage_identifiers{};
        const auto initialize_identifier = [&](size_t index, const shader_stage_source& source) -> const void* {
            if (source.identifier.empty())
            {
                return nullptr;
            }
            auto& identifier = stage_identifiers[index];
            identifier.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;
            identifier.identifierSize = static_cast<uint32_t>(source.identifier.size());
            identifier.pIdentifier = source.identifier.data();
            return &identifier;
        };

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].pNext = initialize_identifier(0, vertex_shader);
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[0].pSpecializationInfo = build_spec(vs_spec, 0);
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].pNext = initialize_identifier(1, fragment_shader);
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
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

        std::vector<VkVertexInputBindingDivisorDescription> vk_divisors;
        vk_divisors.reserve(divisors.size());
        for (const vertex_divisor& divisor : divisors)
        {
            const auto binding =
                std::ranges::find_if(bindings, [&](const vertex_binding& candidate) { return candidate.binding == divisor.binding; });
            if (binding == bindings.end() || binding->input_rate != VK_VERTEX_INPUT_RATE_INSTANCE ||
                std::ranges::find_if(vk_divisors, [&](const VkVertexInputBindingDivisorDescription& candidate) {
                    return candidate.binding == divisor.binding;
                }) != vk_divisors.end())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            vk_divisors.push_back({.binding = divisor.binding, .divisor = divisor.divisor});
        }

        VkPipelineVertexInputDivisorStateCreateInfo divisor_state{};
        if (!vk_divisors.empty())
        {
            divisor_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO;
            divisor_state.vertexBindingDivisorCount = static_cast<uint32_t>(vk_divisors.size());
            divisor_state.pVertexBindingDivisors = vk_divisors.data();
        }

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.pNext = vk_divisors.empty() ? nullptr : &divisor_state;
        vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(vk_bindings.size());
        vertex_input.pVertexBindingDescriptions = vk_bindings.empty() ? nullptr : vk_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(vk_attributes.size());
        vertex_input.pVertexAttributeDescriptions = vk_attributes.empty() ? nullptr : vk_attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = static_cast<VkPrimitiveTopology>(primitive_topology);
        input_assembly.primitiveRestartEnable = primitive_restart_enable ? VK_TRUE : VK_FALSE;

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
        const bool viewport_with_count =
            std::ranges::find(dynamic_states, static_cast<uint32_t>(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT)) != dynamic_states.end();
        const bool scissor_with_count =
            std::ranges::find(dynamic_states, static_cast<uint32_t>(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT)) != dynamic_states.end();
        viewport_state.viewportCount = viewport_with_count ? 0 : 1;
        viewport_state.pViewports = dynamic_rendering ? nullptr : &viewport;
        viewport_state.scissorCount = scissor_with_count ? 0 : 1;
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

        VkPipelineRasterizationStateStreamCreateInfoEXT rasterization_stream_info{};
        if (rasterization_stream != UINT32_MAX)
        {
            rasterization_stream_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT;
            rasterization_stream_info.flags = static_cast<VkPipelineRasterizationStateStreamCreateFlagsEXT>(rasterization_stream_flags);
            rasterization_stream_info.rasterizationStream = rasterization_stream;
        }

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.pNext = rasterization_stream != UINT32_MAX ? &rasterization_stream_info : nullptr;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
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
        info.flags = static_cast<VkPipelineCreateFlags>(flags);
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
        const VkResult result = dev->second.create_graphics_pipelines(dev->second.handle, cache_handle, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        impl::pipeline_params create{};
        create.pipeline_cache = pipeline_cache;
        create.render_pass = render_pass;
        if (const auto rp = this->impl_->render_passes.find(render_pass); rp != this->impl_->render_passes.end())
        {
            create.render_pass_create = rp->second.create;
        }
        create.pipeline_layout = pipeline_layout;
        create.layout_create = layout->second.create;
        create.vertex = this->impl_->stage_params(vertex_shader);
        create.fragment = this->impl_->stage_params(fragment_shader);
        create.flags = flags;
        create.width = width;
        create.height = height;
        create.bindings.assign(bindings.begin(), bindings.end());
        create.attributes.assign(attributes.begin(), attributes.end());
        create.divisors.assign(divisors.begin(), divisors.end());
        create.depth = depth;
        create.color_formats.assign(color_formats.begin(), color_formats.end());
        create.depth_format = depth_format;
        create.stencil_format = stencil_format;
        create.rasterization_samples = rasterization_samples;
        create.primitive_topology = primitive_topology;
        create.primitive_restart_enable = primitive_restart_enable;
        create.rasterization_stream = rasterization_stream;
        create.rasterization_stream_flags = rasterization_stream_flags;
        create.dynamic_states.assign(dynamic_states.begin(), dynamic_states.end());
        create.vs_spec = {.entries = {vs_spec.entries.begin(), vs_spec.entries.end()}, .data = {vs_spec.data.begin(), vs_spec.data.end()}};
        create.fs_spec = {.entries = {fs_spec.entries.begin(), fs_spec.entries.end()}, .data = {fs_spec.data.begin(), fs_spec.data.end()}};
        create.blend_attachments.assign(blend_attachments_in.begin(), blend_attachments_in.end());
        this->impl_->pipelines.emplace(id, impl::pipeline_data{.handle = pipeline, .device_id = device, .create = std::move(create)});
        out_pipeline = id;
        return VK_SUCCESS;
    }

    int32_t vulkan_host::create_compute_pipeline(uint64_t device, uint64_t pipeline_cache, uint64_t pipeline_layout,
                                                 const shader_stage_source& shader, uint32_t flags, uint64_t& out_pipeline)
    {
        out_pipeline = 0;

        const auto dev = this->impl_->devices.find(device);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (dev == this->impl_->devices.end() || layout == this->impl_->pipeline_layouts.end() || layout->second.device_id != device ||
            !dev->second.create_compute_pipelines)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const bool has_module = shader.module != 0;
        const bool has_identifier = !shader.identifier.empty();
        if (has_module == has_identifier || shader.identifier.size() > VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkShaderModule module = VK_NULL_HANDLE;
        if (has_module)
        {
            const auto module_it = this->impl_->shader_modules.find(shader.module);
            if (module_it == this->impl_->shader_modules.end() || module_it->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            module = module_it->second.handle;
        }

        VkPipelineShaderStageModuleIdentifierCreateInfoEXT identifier{};
        if (has_identifier)
        {
            identifier.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;
            identifier.identifierSize = static_cast<uint32_t>(shader.identifier.size());
            identifier.pIdentifier = shader.identifier.data();
        }

        VkPipelineCache cache_handle = VK_NULL_HANDLE;
        if (pipeline_cache != 0)
        {
            const auto cache = this->impl_->pipeline_caches.find(pipeline_cache);
            if (cache == this->impl_->pipeline_caches.end() || cache->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            cache_handle = cache->second.handle;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.flags = static_cast<VkPipelineCreateFlags>(flags);
        info.layout = layout->second.handle;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.pNext = has_identifier ? &identifier : nullptr;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";

        VkPipeline pipeline{};
        const VkResult result = dev->second.create_compute_pipelines(dev->second.handle, cache_handle, 1, &info, nullptr, &pipeline);
        if (result != VK_SUCCESS)
        {
            return result;
        }

        const uint64_t id = this->impl_->allocate_id();
        impl::pipeline_params create{};
        create.compute = true;
        create.pipeline_cache = pipeline_cache;
        create.pipeline_layout = pipeline_layout;
        create.layout_create = layout->second.create;
        create.vertex = this->impl_->stage_params(shader);
        create.flags = flags;
        this->impl_->pipelines.emplace(id, impl::pipeline_data{.handle = pipeline, .device_id = device, .create = std::move(create)});
        out_pipeline = id;
        return VK_SUCCESS;
    }

    void vulkan_host::destroy_pipeline(uint64_t device, uint64_t pipeline)
    {
        const auto dev = this->impl_->devices.find(device);
        const auto it = this->impl_->pipelines.find(pipeline);
        if (it == this->impl_->pipelines.end() || it->second.device_id != device)
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
        if (cb == this->impl_->command_buffers.end() || rp == this->impl_->render_passes.end() || fb == this->impl_->framebuffers.end() ||
            rp->second.device_id != cb->second.device_id || fb->second.device_id != cb->second.device_id)
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

        const auto record_transition = [&](const uint64_t view_id, const uint32_t aspect_mask, const VkImageLayout layout) {
            const auto view = this->impl_->image_views.find(view_id);
            if (view == this->impl_->image_views.end())
            {
                return;
            }

            const auto& create = view->second.create;
            cb->second.layout_transitions.push_back(impl::layout_transition{
                .image = create.image,
                .aspect_mask = aspect_mask,
                .base_mip_level = create.base_mip_level,
                .level_count = create.level_count ? create.level_count : 1,
                .base_array_layer = create.base_array_layer,
                .layer_count = create.layer_count ? create.layer_count : 1,
                .new_layout = static_cast<uint32_t>(layout),
            });
        };

        record_transition(fb->second.create.image_view, VK_IMAGE_ASPECT_COLOR_BIT, translate_layout(rp->second.create.final_layout));
        if (fb->second.create.depth_view != 0)
        {
            record_transition(fb->second.create.depth_view, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_pipeline(uint64_t command_buffer, uint64_t pipeline, uint32_t bind_point)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto pipe = this->impl_->pipelines.find(pipeline);
        if (cb == this->impl_->command_buffers.end() || pipe == this->impl_->pipelines.end() ||
            pipe->second.device_id != cb->second.device_id)
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
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
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
            if (buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
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

        static thread_local std::vector<VkBuffer> handles;
        static thread_local std::vector<VkDeviceSize> vk_offsets;
        static thread_local std::vector<VkDeviceSize> vk_sizes;
        static thread_local std::vector<VkDeviceSize> vk_strides;
        handles.resize(count);
        vk_offsets.resize(count);
        vk_sizes.resize(count);
        vk_strides.resize(count);
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
            if (buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_bind_index_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t size,
                                               uint32_t index_type)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (dev->second.maintenance5_enabled && dev->second.cmd_bind_index_buffer2)
        {
            dev->second.cmd_bind_index_buffer2(cb->second.handle, buf->second.handle, offset, size, static_cast<VkIndexType>(index_type));
        }
        else if (size == VK_WHOLE_SIZE && dev->second.cmd_bind_index_buffer)
        {
            dev->second.cmd_bind_index_buffer(cb->second.handle, buf->second.handle, offset, static_cast<VkIndexType>(index_type));
        }
        else
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
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

    int32_t vulkan_host::cmd_draw_indexed_indirect(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint32_t draw_count,
                                                   uint32_t stride)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indexed_indirect)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_draw_indexed_indirect(cb->second.handle, buf->second.handle, offset, draw_count, stride);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw_indexed_indirect_count(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t count_buffer,
                                                         uint64_t count_buffer_offset, uint32_t max_draw_count, uint32_t stride)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto indirect_buffer = this->impl_->buffers.find(buffer);
        const auto draw_count_buffer = this->impl_->buffers.find(count_buffer);
        if (cb == this->impl_->command_buffers.end() || indirect_buffer == this->impl_->buffers.end() ||
            draw_count_buffer == this->impl_->buffers.end() || indirect_buffer->second.device_id != cb->second.device_id ||
            draw_count_buffer->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indexed_indirect_count)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_draw_indexed_indirect_count(cb->second.handle, indirect_buffer->second.handle, offset,
                                                    draw_count_buffer->second.handle, count_buffer_offset, max_draw_count, stride);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw_indirect(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto buf = this->impl_->buffers.find(buffer);
        if (cb == this->impl_->command_buffers.end() || buf == this->impl_->buffers.end() || buf->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indirect)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_draw_indirect(cb->second.handle, buf->second.handle, offset, draw_count, stride);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_draw_indirect_count(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t count_buffer,
                                                 uint64_t count_buffer_offset, uint32_t max_draw_count, uint32_t stride)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto indirect_buffer = this->impl_->buffers.find(buffer);
        const auto draw_count_buffer = this->impl_->buffers.find(count_buffer);
        if (cb == this->impl_->command_buffers.end() || indirect_buffer == this->impl_->buffers.end() ||
            draw_count_buffer == this->impl_->buffers.end() || indirect_buffer->second.device_id != cb->second.device_id ||
            draw_count_buffer->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_draw_indirect_count)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_draw_indirect_count(cb->second.handle, indirect_buffer->second.handle, offset, draw_count_buffer->second.handle,
                                            count_buffer_offset, max_draw_count, stride);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::cmd_bind_descriptor_sets(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t first_set,
                                                  std::span<const uint64_t> sets, uint32_t bind_point,
                                                  std::span<const uint32_t> dynamic_offsets)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end() ||
            layout->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_bind_descriptor_sets)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        static thread_local std::vector<VkDescriptorSet> handles;
        handles.clear();
        for (const uint64_t id : sets)
        {
            const auto set = this->impl_->descriptor_sets.find(id);
            if (set == this->impl_->descriptor_sets.end() || set->second.device_id != cb->second.device_id)
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

    int32_t vulkan_host::cmd_next_subpass(uint64_t command_buffer, uint32_t contents)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_next_subpass)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        dev->second.cmd_next_subpass(cb->second.handle, static_cast<VkSubpassContents>(contents));
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

        bool valid_views = true;
        const auto view_handle = [&](uint64_t id) -> VkImageView {
            if (id == 0)
            {
                return VK_NULL_HANDLE;
            }
            const auto it = this->impl_->image_views.find(id);
            if (it == this->impl_->image_views.end() || it->second.device_id != cb->second.device_id)
            {
                valid_views = false;
                return VK_NULL_HANDLE;
            }
            return it->second.handle;
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
        if (!valid_views)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
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
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end() ||
            layout->second.device_id != cb->second.device_id)
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
        case gpu_bridge::dynamic_state_u32::depth_clip_enable:
            if (!dev->second.cmd_set_depth_clip_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_depth_clip_enable(handle, value);
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

    namespace
    {
        constexpr uint32_t snapshot_version = 10;

        template <typename Map>
        void reject_live_objects(const Map& objects, const std::string_view type)
        {
            if (!objects.empty())
            {
                throw std::runtime_error("Snapshots do not support live Vulkan " + std::string{type} + " yet");
            }
        }

        void expect_restored(const int32_t result, const uint64_t restored, const uint64_t expected, const std::string_view type)
        {
            if (result != VK_SUCCESS || restored != expected)
            {
                throw std::runtime_error("Restoring Vulkan " + std::string{type} + " " + std::to_string(expected) + " failed with result " +
                                         std::to_string(result));
            }
        }

        // Moves the contents of memory the host cannot map through a host-visible staging buffer, by aliasing transfer
        // buffers over the memory one chunk at a time. Only for snapshots, while the guest is stopped.
        template <typename Device>
        class snapshot_transfer
        {
          public:
            static constexpr VkDeviceSize chunk_size = 16 * 1024 * 1024;

            snapshot_transfer(const Device& device, const std::function<uint32_t(uint32_t type_bits)>& find_host_memory_type)
                : device_(device)
            {
                try
                {
                    this->create(find_host_memory_type);
                }
                catch (...)
                {
                    this->release();
                    throw;
                }
            }

            ~snapshot_transfer()
            {
                this->release();
            }

            snapshot_transfer(const snapshot_transfer&) = delete;
            snapshot_transfer& operator=(const snapshot_transfer&) = delete;
            snapshot_transfer(snapshot_transfer&&) = delete;
            snapshot_transfer& operator=(snapshot_transfer&&) = delete;

            // False if the memory's type cannot back transfer buffers, so that only its images can be saved.
            bool copy(const VkDeviceMemory memory, const uint32_t memory_type_index, const std::span<std::byte> contents,
                      const bool to_device)
            {
                const auto& device = this->device_;
                for (VkDeviceSize offset = 0; offset < contents.size(); offset += chunk_size)
                {
                    const VkDeviceSize size = std::min<VkDeviceSize>(chunk_size, contents.size() - offset);

                    // A buffer can need more memory than its size, more than is left at the end of the allocation. It is then
                    // bound further down, and the copy starts inside it.
                    VkDeviceSize alias_offset = offset;
                    VkBuffer alias{};
                    VkMemoryRequirements requirements{};
                    for (int attempt = 0; attempt < 2; ++attempt)
                    {
                        alias = this->create_transfer_buffer(size + (offset - alias_offset));
                        device.get_buffer_memory_requirements(device.handle, alias, &requirements);
                        if ((requirements.memoryTypeBits & (1U << memory_type_index)) == 0 || requirements.alignment == 0 ||
                            requirements.size > contents.size())
                        {
                            device.destroy_buffer(device.handle, alias, nullptr);
                            return false;
                        }

                        const VkDeviceSize highest_offset =
                            (contents.size() - requirements.size) / requirements.alignment * requirements.alignment;
                        const VkDeviceSize aligned_offset = alias_offset / requirements.alignment * requirements.alignment;
                        const VkDeviceSize fitting_offset = std::min(aligned_offset, highest_offset);
                        if (fitting_offset == alias_offset)
                        {
                            break;
                        }

                        device.destroy_buffer(device.handle, alias, nullptr);
                        alias = VK_NULL_HANDLE;
                        alias_offset = fitting_offset;
                    }

                    if (!alias)
                    {
                        return false;
                    }

                    const auto destroy_alias = utils::finally([&] { device.destroy_buffer(device.handle, alias, nullptr); });
                    if (device.bind_buffer_memory(device.handle, alias, memory, alias_offset) != VK_SUCCESS)
                    {
                        throw std::runtime_error("Aliasing Vulkan memory for a snapshot failed");
                    }

                    if (to_device)
                    {
                        std::memcpy(this->staging_data_, contents.data() + offset, static_cast<size_t>(size));
                    }

                    const VkDeviceSize inside = offset - alias_offset;
                    const VkBufferCopy region{.srcOffset = to_device ? 0 : inside, .dstOffset = to_device ? inside : 0, .size = size};
                    this->submit([&](const VkCommandBuffer command_buffer) {
                        device.cmd_copy_buffer(command_buffer, to_device ? this->staging_buffer_ : alias,
                                               to_device ? alias : this->staging_buffer_, 1, &region);
                    });

                    if (!to_device)
                    {
                        std::memcpy(contents.data() + offset, this->staging_data_, static_cast<size_t>(size));
                    }
                }

                return true;
            }

            // Copies one subresource in rows of texel blocks that fit the staging buffer. The image stays in layout, or, when
            // writing, goes from undefined to it. False if a single row does not fit.
            // Barriers cover barrier_aspects, which for combined depth/stencil images has to be both. When writing, from is the
            // layout the image is in before, and the image only goes to layout at the end if finish is set.
            bool copy_image(const VkImage image, const VkImageAspectFlagBits aspect, const VkImageAspectFlags barrier_aspects,
                            const uint32_t mip, const uint32_t layer, const VkExtent3D extent, const vulkan_format_block& block,
                            const VkImageLayout from, const VkImageLayout layout, const bool finish, const std::span<std::byte> contents,
                            const bool to_device)
            {
                const auto& device = this->device_;
                const uint64_t blocks_per_row = (extent.width + block.width - 1) / block.width;
                const uint64_t rows_per_slice = (extent.height + block.height - 1) / block.height;
                const uint64_t row_size = blocks_per_row * block.size;
                if (row_size == 0 || row_size > chunk_size || contents.size() != row_size * rows_per_slice * extent.depth)
                {
                    return false;
                }

                const uint64_t rows_per_chunk = chunk_size / row_size;
                const VkImageLayout transfer_layout =
                    to_device ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                VkImageLayout current = to_device ? from : layout;

                const auto barrier = [&](const VkCommandBuffer command_buffer, const VkImageLayout from, const VkImageLayout to) {
                    VkImageMemoryBarrier info{};
                    info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    info.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    info.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    info.oldLayout = from;
                    info.newLayout = to;
                    info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    info.image = image;
                    info.subresourceRange = {
                        .aspectMask = barrier_aspects, .baseMipLevel = mip, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1};
                    device.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                                0, nullptr, 0, nullptr, 1, &info);
                };

                uint64_t offset = 0;
                for (uint32_t z = 0; z < extent.depth; ++z)
                {
                    for (uint64_t row = 0; row < rows_per_slice; row += rows_per_chunk)
                    {
                        const uint64_t rows = std::min(rows_per_chunk, rows_per_slice - row);
                        const auto y = static_cast<uint32_t>(row * block.height);
                        const auto height = std::min(extent.height - y, static_cast<uint32_t>(rows * block.height));
                        const auto size = static_cast<size_t>(rows * row_size);
                        const bool last = z + 1 == extent.depth && row + rows == rows_per_slice;

                        if (to_device)
                        {
                            std::memcpy(this->staging_data_, contents.data() + offset, size);
                        }

                        VkBufferImageCopy region{};
                        region.imageSubresource = {.aspectMask = static_cast<VkImageAspectFlags>(aspect),
                                                   .mipLevel = mip,
                                                   .baseArrayLayer = layer,
                                                   .layerCount = 1};
                        region.imageOffset = {.x = 0, .y = static_cast<int32_t>(y), .z = static_cast<int32_t>(z)};
                        region.imageExtent = {.width = extent.width, .height = height, .depth = 1};

                        this->submit([&](const VkCommandBuffer command_buffer) {
                            barrier(command_buffer, current, transfer_layout);
                            if (to_device)
                            {
                                device.cmd_copy_buffer_to_image(command_buffer, this->staging_buffer_, image, transfer_layout, 1, &region);
                            }
                            else
                            {
                                device.cmd_copy_image_to_buffer(command_buffer, image, transfer_layout, this->staging_buffer_, 1, &region);
                            }

                            if (!to_device || (last && finish))
                            {
                                barrier(command_buffer, transfer_layout, layout);
                            }
                        });
                        current = to_device ? transfer_layout : layout;

                        if (!to_device)
                        {
                            std::memcpy(contents.data() + offset, this->staging_data_, size);
                        }

                        offset += size;
                    }
                }

                return true;
            }

            void reset_query_pool(const VkQueryPool pool, const uint32_t count)
            {
                this->submit(
                    [&](const VkCommandBuffer command_buffer) { this->device_.cmd_reset_query_pool(command_buffer, pool, 0, count); });
            }

          private:
            const Device& device_;
            VkQueue queue_{};
            VkCommandPool pool_{};
            VkCommandBuffer command_buffer_{};
            VkBuffer staging_buffer_{};
            VkDeviceMemory staging_memory_{};
            void* staging_data_{};

            void create(const std::function<uint32_t(uint32_t type_bits)>& find_host_memory_type)
            {
                const auto& device = this->device_;
                device.get_device_queue(device.handle, device.queue_family_index, 0, &this->queue_);

                VkCommandPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_info.queueFamilyIndex = device.queue_family_index;

                VkCommandBufferAllocateInfo command_buffer_info{};
                command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                command_buffer_info.commandBufferCount = 1;

                if (!this->queue_ || device.create_command_pool(device.handle, &pool_info, nullptr, &this->pool_) != VK_SUCCESS ||
                    (command_buffer_info.commandPool = this->pool_,
                     device.allocate_command_buffers(device.handle, &command_buffer_info, &this->command_buffer_)) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot transfer queue failed");
                }

                this->staging_buffer_ = this->create_transfer_buffer(chunk_size);
                VkMemoryRequirements requirements{};
                device.get_buffer_memory_requirements(device.handle, this->staging_buffer_, &requirements);

                VkMemoryAllocateInfo allocate_info{};
                allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocate_info.allocationSize = requirements.size;
                allocate_info.memoryTypeIndex = find_host_memory_type(requirements.memoryTypeBits);
                if (allocate_info.memoryTypeIndex == UINT32_MAX ||
                    device.allocate_memory(device.handle, &allocate_info, nullptr, &this->staging_memory_) != VK_SUCCESS ||
                    device.bind_buffer_memory(device.handle, this->staging_buffer_, this->staging_memory_, 0) != VK_SUCCESS ||
                    device.map_memory(device.handle, this->staging_memory_, 0, VK_WHOLE_SIZE, 0, &this->staging_data_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot staging buffer failed");
                }
            }

            void release()
            {
                const auto& device = this->device_;
                if (this->staging_memory_)
                {
                    device.free_memory(device.handle, this->staging_memory_, nullptr);
                }
                if (this->staging_buffer_)
                {
                    device.destroy_buffer(device.handle, this->staging_buffer_, nullptr);
                }
                if (this->pool_)
                {
                    device.destroy_command_pool(device.handle, this->pool_, nullptr);
                }

                this->staging_memory_ = VK_NULL_HANDLE;
                this->staging_buffer_ = VK_NULL_HANDLE;
                this->pool_ = VK_NULL_HANDLE;
            }

            VkBuffer create_transfer_buffer(const VkDeviceSize size) const
            {
                VkBufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size = size;
                info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

                VkBuffer buffer{};
                if (this->device_.create_buffer(this->device_.handle, &info, nullptr, &buffer) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating a Vulkan snapshot transfer buffer failed");
                }
                return buffer;
            }

            template <typename Record>
            void submit(const Record& record)
            {
                const auto& device = this->device_;

                VkCommandBufferBeginInfo begin_info{};
                begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

                VkSubmitInfo submit_info{};
                submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit_info.commandBufferCount = 1;
                submit_info.pCommandBuffers = &this->command_buffer_;

                if (device.begin_command_buffer(this->command_buffer_, &begin_info) != VK_SUCCESS)
                {
                    throw std::runtime_error("Recording a Vulkan snapshot transfer failed");
                }

                record(this->command_buffer_);

                if (device.end_command_buffer(this->command_buffer_) != VK_SUCCESS ||
                    device.queue_submit(this->queue_, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS ||
                    device.queue_wait_idle(this->queue_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Running a Vulkan snapshot transfer failed");
                }
            }
        };

        std::vector<VkPhysicalDevice> enumerate_native_physical_devices(const PFN_vkEnumeratePhysicalDevices enumerate,
                                                                        const VkInstance instance)
        {
            uint32_t count = 0;
            if (!enumerate || enumerate(instance, &count, nullptr) != VK_SUCCESS)
            {
                return {};
            }

            std::vector<VkPhysicalDevice> devices(count);
            if (enumerate(instance, &count, devices.data()) != VK_SUCCESS)
            {
                return {};
            }

            devices.resize(count);
            return devices;
        }
    }

    void vulkan_host::serialize(utils::buffer_serializer& buffer) const
    {
        auto& impl = *this->impl_;

        for (const auto& memory : impl.memories | std::views::values)
        {
            if (memory.placed)
            {
                throw std::runtime_error("Snapshots do not support placed Vulkan memory mappings yet");
            }

            if (memory.imported && memory.non_coherent)
            {
                throw std::runtime_error("Snapshots do not support non-coherent imported Vulkan memory yet");
            }
        }

        // Waiting for a device to go idle never returns while a queue waits for something only later guest work would
        // signal.
        for (const auto& semaphore : impl.semaphores | std::views::values)
        {
            if (semaphore.binary_wait_unsignaled)
            {
                throw std::runtime_error("A Vulkan queue waits for a binary semaphore nothing has been submitted to signal");
            }

            if (semaphore.type != VK_SEMAPHORE_TYPE_TIMELINE)
            {
                continue;
            }

            const auto& device = impl.devices.at(semaphore.device_id);
            uint64_t value = 0;
            if (!device.get_semaphore_counter_value ||
                device.get_semaphore_counter_value(device.handle, semaphore.handle, &value) != VK_SUCCESS)
            {
                throw std::runtime_error("Reading a Vulkan timeline semaphore failed");
            }

            if (semaphore.highest_wait > std::max(value, semaphore.highest_signal))
            {
                throw std::runtime_error("A Vulkan queue waits for a timeline value nothing has been submitted to signal");
            }
        }

        for (auto& device : impl.devices | std::views::values)
        {
            std::erase_if(device.abandoned_drain_fences, [&](const VkFence fence) {
                if (device.get_fence_status(device.handle, fence) != VK_SUCCESS)
                {
                    return false;
                }

                device.destroy_fence(device.handle, fence, nullptr);
                return true;
            });
        }

        // The checks above cannot see the order of submissions, e.g. a wait on one queue for a signal submitted after it on
        // the same queue. Each queue therefore gets a fence behind its work, which signals once all of it is done, and the
        // snapshot is refused unless every fence does so in time.
        std::unordered_set<VkQueue> drained{};
        for (const auto& queue : impl.queues | std::views::values)
        {
            auto& device = impl.devices.at(queue.device_id);
            if (!drained.insert(queue.handle).second)
            {
                continue;
            }

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence fence{};
            if (device.create_fence(device.handle, &fence_info, nullptr, &fence) != VK_SUCCESS)
            {
                throw std::runtime_error("Creating a Vulkan fence to drain a queue failed");
            }

            if (device.queue_submit(queue.handle, 0, nullptr, fence) != VK_SUCCESS)
            {
                device.destroy_fence(device.handle, fence, nullptr);
                throw std::runtime_error("Draining a Vulkan queue failed");
            }

            constexpr auto drain_timeout = std::chrono::seconds(5);
            const auto deadline = std::chrono::steady_clock::now() + drain_timeout;
            VkResult status = VK_NOT_READY;
            while ((status = device.get_fence_status(device.handle, fence)) == VK_NOT_READY && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (status != VK_SUCCESS)
            {
                device.abandoned_drain_fences.push_back(fence);
                throw std::runtime_error(
                    "A Vulkan queue did not finish its work, it may wait for something the guest has not signaled yet");
            }

            device.destroy_fence(device.handle, fence, nullptr);
        }

        for (const auto& device : impl.devices | std::views::values)
        {
            if (!device.device_wait_idle || device.device_wait_idle(device.handle) != VK_SUCCESS)
            {
                throw std::runtime_error("Waiting for a Vulkan device to go idle failed");
            }
        }

        std::unordered_map<uint64_t, std::unique_ptr<snapshot_transfer<impl::device_data>>> transfers{};
        const auto transfer_for = [&](const uint64_t device_id) -> snapshot_transfer<impl::device_data>& {
            auto& transfer = transfers[device_id];
            if (!transfer)
            {
                const auto& device = impl.devices.at(device_id);
                transfer = std::make_unique<snapshot_transfer<impl::device_data>>(device, [&](const uint32_t type_bits) {
                    return impl.find_memory_type(device, type_bits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                });
            }
            return *transfer;
        };

        const auto memory_property_flags = [&](const impl::device_data& device, const uint32_t memory_type_index) -> VkMemoryPropertyFlags {
            VkPhysicalDeviceMemoryProperties properties{};
            if (!impl.query_memory_properties(device, properties))
            {
                return 0;
            }

            const auto index = impl.substitute_cached_memory_type(device, memory_type_index);
            return index < properties.memoryTypeCount ? properties.memoryTypes[index].propertyFlags : 0;
        };

        buffer.write(snapshot_version);
        buffer.write(impl.next_id);

        buffer.write(static_cast<uint64_t>(impl.instances.size()));
        for (const auto& [id, instance] : impl.instances)
        {
            buffer.write(id);
            buffer.write(static_cast<uint64_t>(instance.extensions.size()));
            for (const auto& extension : instance.extensions)
            {
                buffer.write_string(extension);
            }
        }

        buffer.write(static_cast<uint64_t>(impl.physical_devices.size()));
        for (const auto& [id, physical_device] : impl.physical_devices)
        {
            const auto& instance = impl.instances.at(physical_device.instance_id);
            const auto native = enumerate_native_physical_devices(instance.enumerate_physical_devices, instance.handle);
            const auto position = std::ranges::find(native, physical_device.handle);
            if (position == native.end() || !instance.get_physical_device_properties)
            {
                throw std::runtime_error("A Vulkan physical device is no longer enumerated");
            }

            VkPhysicalDeviceProperties properties{};
            instance.get_physical_device_properties(physical_device.handle, &properties);

            buffer.write(id);
            buffer.write(physical_device.instance_id);
            buffer.write(static_cast<uint32_t>(position - native.begin()));
            buffer.write(properties.vendorID);
            buffer.write(properties.deviceID);
        }

        buffer.write(static_cast<uint64_t>(impl.devices.size()));
        for (const auto& [id, device] : impl.devices)
        {
            buffer.write(id);
            buffer.write(device.create.physical_device);
            buffer.write_vector(device.create.queues);
            buffer.write_vector(device.create.extensions);
            buffer.write(device.create.extension_count);
            buffer.write_vector(device.create.features);
            buffer.write(device.create.feature_count);
            buffer.write(device.create.spoof_missing_features);
        }

        buffer.write(static_cast<uint64_t>(impl.queues.size()));
        for (const auto& [id, queue] : impl.queues)
        {
            buffer.write(id);
            buffer.write(queue.device_id);
            buffer.write(queue.queue_family_index);
            buffer.write(queue.queue_index);
        }

        buffer.write(static_cast<uint64_t>(impl.memories.size()));
        for (const auto& [id, memory] : impl.memories)
        {
            buffer.write(id);
            buffer.write(memory.device_id);
            buffer.write(memory.requested_size);
            buffer.write(memory.memory_type_index);
            buffer.write(memory.flags);
            buffer.write(memory.device_mask);
            buffer.write(memory.imported);

            if (memory.imported)
            {
                continue;
            }

            const auto& device = impl.devices.at(memory.device_id);
            const auto property_flags = memory_property_flags(device, memory.memory_type_index);
            if ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            {
                std::vector<std::byte> contents(memory.allocation_size);
                if (!transfer_for(memory.device_id)
                         .copy(memory.handle, impl.substitute_cached_memory_type(device, memory.memory_type_index), contents, false))
                {
                    throw std::runtime_error("Snapshots do not save Vulkan memory only images can use yet");
                }

                buffer.write_vector(contents);
                continue;
            }

            if ((property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            {
                throw std::runtime_error("Snapshots do not save non-coherent Vulkan memory yet");
            }

            void* mapped = memory.persistent_host_pointer;
            if (!mapped &&
                (!device.map_memory || device.map_memory(device.handle, memory.handle, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS))
            {
                throw std::runtime_error("Mapping Vulkan memory for a snapshot failed");
            }

            const auto* contents = static_cast<const std::byte*>(mapped);
            buffer.write_vector(std::vector<std::byte>(contents, contents + memory.allocation_size));

            if (!memory.persistent_host_pointer)
            {
                device.unmap_memory(device.handle, memory.handle);
            }
        }

        buffer.write(static_cast<uint64_t>(impl.buffers.size()));
        for (const auto& [id, buffer_object] : impl.buffers)
        {
            buffer.write(id);
            buffer.write(buffer_object.device_id);
            buffer.write(buffer_object.size);
            buffer.write(buffer_object.usage);
            buffer.write(buffer_object.memory_id);
            buffer.write(buffer_object.memory_offset);
        }

        buffer.write(static_cast<uint64_t>(std::ranges::count_if(impl.images, [](const auto& entry) { return entry.second.owned; })));
        for (const auto& [id, image] : impl.images)
        {
            if (!image.owned)
            {
                continue;
            }

            const auto& create = image.create;
            buffer.write(id);
            buffer.write(image.device_id);
            buffer.write(create);
            buffer.write(image.memory_id);
            buffer.write(image.memory_offset);
            buffer.write_vector(image.layouts);

            const auto aspects = get_vulkan_format_aspects(create.format);
            uint32_t aspect = 0;
            for (uint32_t bit = 1; bit <= VK_IMAGE_ASPECT_STENCIL_BIT; bit <<= 1)
            {
                if ((aspects & bit) == 0)
                {
                    continue;
                }

                for (uint32_t mip = 0; mip < create.mip_levels; ++mip)
                {
                    for (uint32_t layer = 0; layer < create.array_layers; ++layer)
                    {
                        const auto layout = static_cast<VkImageLayout>(image.layouts[image.subresource_index(aspect, mip, layer)]);
                        if (layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED || image.memory_id == 0)
                        {
                            continue;
                        }

                        const auto block = get_vulkan_copy_block(create.format, bit);
                        const bool layouts_match =
                            aspect == 0 || layout == static_cast<VkImageLayout>(image.layouts[image.subresource_index(0, mip, layer)]);
                        if (!image.copyable || image.samples > 1 || image.ownership_transferred || image.used_by_several_queues || !block ||
                            !layouts_match)
                        {
                            throw std::runtime_error("Snapshots cannot save the contents of Vulkan image " + std::to_string(id) + " yet");
                        }

                        const VkExtent3D extent{.width = std::max(1U, create.width >> mip),
                                                .height = std::max(1U, create.height >> mip),
                                                .depth = std::max(1U, create.depth >> mip)};
                        const uint64_t size = ((extent.width + block->width - 1) / block->width) *
                                              ((extent.height + block->height - 1) / block->height) * extent.depth * block->size;

                        std::vector<std::byte> contents(size);
                        if (!transfer_for(image.device_id)
                                 .copy_image(image.handle, static_cast<VkImageAspectFlagBits>(bit), aspects, mip, layer, extent, *block,
                                             layout, layout, true, contents, false))
                        {
                            throw std::runtime_error("Snapshots cannot save the contents of Vulkan image " + std::to_string(id) + " yet");
                        }

                        buffer.write(true);
                        buffer.write(bit);
                        buffer.write(mip);
                        buffer.write(layer);
                        buffer.write_vector(contents);
                    }
                }

                ++aspect;
            }

            buffer.write(false);
        }

        buffer.write(static_cast<uint64_t>(impl.surfaces.size()));
        for (const auto& [id, surface] : impl.surfaces)
        {
            buffer.write(id);
            buffer.write(surface.instance_id);
        }

        // Re-created swapchains give the guest images with undefined contents, so it has to draw each frame anew.
        buffer.write(static_cast<uint64_t>(impl.swapchains.size()));
        for (const auto& [id, swapchain] : impl.swapchains)
        {
            if (std::ranges::any_of(swapchain.acquired, [](const bool acquired) { return acquired; }))
            {
                throw std::runtime_error("Snapshots cannot be taken while the guest holds a swapchain image");
            }

            if (std::ranges::count_if(impl.swapchains, [&](const auto& other) { return other.second.surface_id == swapchain.surface_id; }) >
                1)
            {
                throw std::runtime_error("Snapshots do not support a retired Vulkan swapchain that is still alive yet");
            }

            buffer.write(id);
            buffer.write(swapchain.device_id);
            buffer.write(swapchain.surface_id);
            buffer.write(swapchain.create);
            buffer.write_vector(swapchain.image_ids);
            for (const auto image : swapchain.image_ids)
            {
                buffer.write_vector(impl.images.at(image).layouts);
            }
        }

        buffer.write(static_cast<uint64_t>(impl.query_pools.size()));
        for (const auto& [id, pool] : impl.query_pools)
        {
            const auto& device = impl.devices.at(pool.device_id);
            const uint32_t values = pool.value_count();
            std::vector<uint64_t> results(static_cast<size_t>(pool.query_count) * (values + 1));
            const VkResult result =
                pool.query_count == 0
                    ? VK_SUCCESS
                    : device.get_query_pool_results(device.handle, pool.handle, 0, pool.query_count, results.size() * sizeof(uint64_t),
                                                    results.data(), (values + 1) * sizeof(uint64_t),
                                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS && result != VK_NOT_READY)
            {
                throw std::runtime_error("Reading Vulkan query pool " + std::to_string(id) + " failed");
            }

            auto available = pool.restored_results;
            for (uint32_t query = 0; query < pool.query_count; ++query)
            {
                const auto* entry = results.data() + (static_cast<size_t>(query) * (values + 1));
                if (entry[values] != 0)
                {
                    available[query].assign(entry, entry + values);
                }
            }

            buffer.write(id);
            buffer.write(pool.device_id);
            buffer.write(pool.query_type);
            buffer.write(pool.query_count);
            buffer.write(pool.pipeline_statistics);
            buffer.write(static_cast<uint64_t>(available.size()));
            for (const auto& [query, query_values] : available)
            {
                buffer.write(query);
                buffer.write_vector(query_values);
            }
        }

        buffer.write(static_cast<uint64_t>(impl.samplers.size()));
        for (const auto& [id, sampler] : impl.samplers)
        {
            buffer.write(id);
            buffer.write(sampler.device_id);
            buffer.write(sampler.create);
        }

        buffer.write(static_cast<uint64_t>(impl.shader_modules.size()));
        for (const auto& [id, module] : impl.shader_modules)
        {
            buffer.write(id);
            buffer.write(module.device_id);
            buffer.write(*module.code);
        }

        buffer.write(static_cast<uint64_t>(impl.identified_code.size()));
        for (const auto& [identifier, code] : impl.identified_code)
        {
            buffer.write_string(identifier);
            buffer.write(*code);
        }

        buffer.write(static_cast<uint64_t>(impl.descriptor_set_layouts.size()));
        for (const auto& [id, layout] : impl.descriptor_set_layouts)
        {
            buffer.write(id);
            buffer.write(layout.device_id);
            buffer.write(*layout.create);
        }

        buffer.write(static_cast<uint64_t>(impl.pipeline_layouts.size()));
        for (const auto& [id, layout] : impl.pipeline_layouts)
        {
            buffer.write(id);
            buffer.write(layout.device_id);
            buffer.write(*layout.create);
        }

        buffer.write(static_cast<uint64_t>(impl.render_passes.size()));
        for (const auto& [id, render_pass] : impl.render_passes)
        {
            buffer.write(id);
            buffer.write(render_pass.device_id);
            buffer.write(render_pass.create);
        }

        buffer.write(static_cast<uint64_t>(impl.pipeline_caches.size()));
        for (const auto& [id, cache] : impl.pipeline_caches)
        {
            const auto& device = impl.devices.at(cache.device_id);
            size_t size = 0;
            if (device.get_pipeline_cache_data(device.handle, cache.handle, &size, nullptr) != VK_SUCCESS)
            {
                throw std::runtime_error("Reading a Vulkan pipeline cache failed");
            }

            std::vector<uint8_t> data(size);
            if (device.get_pipeline_cache_data(device.handle, cache.handle, &size, data.data()) != VK_SUCCESS)
            {
                throw std::runtime_error("Reading a Vulkan pipeline cache failed");
            }

            data.resize(size);
            buffer.write(id);
            buffer.write(cache.device_id);
            buffer.write(cache.flags);
            buffer.write_vector(data);
        }

        buffer.write(static_cast<uint64_t>(impl.pipelines.size()));
        for (const auto& [id, pipeline] : impl.pipelines)
        {
            const auto restorable = [&](const impl::shader_stage_params& stage) {
                return stage.code != nullptr || impl.shader_modules.contains(stage.module);
            };

            if (!restorable(pipeline.create.vertex) || (!pipeline.create.compute && !restorable(pipeline.create.fragment)))
            {
                throw std::runtime_error("Vulkan pipeline " + std::to_string(id) + " was created from shader code the bridge never saw");
            }

            buffer.write(id);
            buffer.write(pipeline.device_id);
            buffer.write(pipeline.create);
        }

        buffer.write(static_cast<uint64_t>(impl.image_views.size()));
        for (const auto& [id, view] : impl.image_views)
        {
            buffer.write(id);
            buffer.write(view.device_id);
            buffer.write(view.create);
        }

        buffer.write(static_cast<uint64_t>(impl.buffer_views.size()));
        for (const auto& [id, view] : impl.buffer_views)
        {
            buffer.write(id);
            buffer.write(view.device_id);
            buffer.write(view.create);
        }

        buffer.write(static_cast<uint64_t>(impl.framebuffers.size()));
        for (const auto& [id, framebuffer] : impl.framebuffers)
        {
            buffer.write(id);
            buffer.write(framebuffer.device_id);
            buffer.write(framebuffer.create);
        }

        buffer.write(static_cast<uint64_t>(impl.descriptor_pools.size()));
        for (const auto& [id, pool] : impl.descriptor_pools)
        {
            buffer.write(id);
            buffer.write(pool.device_id);
            buffer.write(pool.create);
        }

        buffer.write(static_cast<uint64_t>(impl.descriptor_sets.size()));
        for (const auto& [id, set] : impl.descriptor_sets)
        {
            buffer.write(id);
            buffer.write(set.device_id);
            buffer.write(set.pool_id);
            buffer.write(set.layout_id);
            buffer.write(*set.layout_create);
            buffer.write(set.variable_descriptor_count.has_value());
            buffer.write(set.variable_descriptor_count.value_or(0));

            buffer.write(static_cast<uint64_t>(set.slots.size()));
            for (const auto& [key, slot] : set.slots)
            {
                buffer.write(key);
                buffer.write(slot);
            }

            buffer.write(static_cast<uint64_t>(set.inline_blocks.size()));
            for (const auto& [binding, block] : set.inline_blocks)
            {
                buffer.write(binding);
                buffer.write_vector(block);
            }
        }

        buffer.write(static_cast<uint64_t>(impl.events.size()));
        for (const auto& [id, event] : impl.events)
        {
            const auto& device = impl.devices.at(event.device_id);
            buffer.write(id);
            buffer.write(event.device_id);
            buffer.write(event.flags);
            buffer.write(device.get_event_status(device.handle, event.handle) == VK_EVENT_SET);
        }

        buffer.write(static_cast<uint64_t>(impl.command_pools.size()));
        for (const auto& [id, pool] : impl.command_pools)
        {
            buffer.write(id);
            buffer.write(pool.device_id);
            buffer.write(pool.queue_family_index);
            buffer.write(pool.flags);
        }

        buffer.write(static_cast<uint64_t>(impl.command_buffers.size()));
        for (const auto& [id, command_buffer] : impl.command_buffers)
        {
            buffer.write(id);
            buffer.write(command_buffer.device_id);
            buffer.write(command_buffer.pool_id);
            buffer.write(command_buffer.level);
            buffer.write_vector(command_buffer.recorded);
            buffer.write_vector(command_buffer.recorded_results);
            buffer.write(std::ranges::any_of(command_buffer.references, [&](const uint64_t id) { return !impl.is_live(id); }));
        }

        buffer.write(static_cast<uint64_t>(impl.fences.size()));
        for (const auto& [id, fence] : impl.fences)
        {
            const auto& device = impl.devices.at(fence.device_id);
            buffer.write(id);
            buffer.write(fence.device_id);
            buffer.write(device.get_fence_status && device.get_fence_status(device.handle, fence.handle) == VK_SUCCESS);
        }

        buffer.write(static_cast<uint64_t>(impl.semaphores.size()));
        for (const auto& [id, semaphore] : impl.semaphores)
        {
            const auto& device = impl.devices.at(semaphore.device_id);
            uint64_t value = 0;
            if (semaphore.type == VK_SEMAPHORE_TYPE_TIMELINE &&
                (!device.get_semaphore_counter_value ||
                 device.get_semaphore_counter_value(device.handle, semaphore.handle, &value) != VK_SUCCESS))
            {
                throw std::runtime_error("Reading a Vulkan timeline semaphore failed");
            }

            buffer.write(id);
            buffer.write(semaphore.device_id);
            buffer.write(semaphore.flags);
            buffer.write(semaphore.type);
            buffer.write(value);
            buffer.write(semaphore.binary_signaled);
            buffer.write(semaphore.binary_wait_unsignaled);
            buffer.write(semaphore.highest_signal);
            buffer.write(semaphore.highest_wait);
        }
    }

    void vulkan_host::deserialize(utils::buffer_deserializer& buffer, const restore_callbacks& callbacks)
    {
        auto& impl = *this->impl_;
        const auto clear_forced_ids = utils::finally([&] { impl.forced_ids.clear(); });

        std::unordered_map<uint64_t, std::unique_ptr<snapshot_transfer<impl::device_data>>> transfers{};
        const auto transfer_for = [&](const uint64_t device_id) -> snapshot_transfer<impl::device_data>& {
            auto& transfer = transfers[device_id];
            if (!transfer)
            {
                const auto& device = impl.devices.at(device_id);
                transfer = std::make_unique<snapshot_transfer<impl::device_data>>(device, [&](const uint32_t type_bits) {
                    return impl.find_memory_type(device, type_bits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                });
            }
            return *transfer;
        };

        if (buffer.read<uint32_t>() != snapshot_version)
        {
            throw std::runtime_error("Unsupported Vulkan snapshot version");
        }

        impl.next_id = buffer.read<uint64_t>();

        std::vector<std::function<void()>> temporaries{};
        const auto destroy_temporaries = utils::finally([&] {
            for (auto it = temporaries.rbegin(); it != temporaries.rend(); ++it)
            {
                (*it)();
            }
        });

        const auto ensure_set_layout = [&](const uint64_t device, const uint64_t id, const impl::set_layout_params& create) {
            if (impl.descriptor_set_layouts.contains(id))
            {
                return id;
            }

            uint64_t layout{};
            if (this->create_descriptor_set_layout(device, create.flags, create.bindings, layout) != VK_SUCCESS)
            {
                throw std::runtime_error("Re-creating a destroyed Vulkan descriptor set layout failed");
            }
            temporaries.emplace_back([this, device, layout] { this->destroy_descriptor_set_layout(device, layout); });
            return layout;
        };

        const auto set_layout_ids = [&](const uint64_t device, const impl::pipeline_layout_params& create) {
            std::vector<uint64_t> ids{};
            for (size_t i = 0; i < create.set_layouts.size(); ++i)
            {
                ids.push_back(ensure_set_layout(device, create.set_layouts[i], *create.set_layout_creates[i]));
            }
            return ids;
        };

        const auto ensure_pipeline_layout = [&](const uint64_t device, const uint64_t id, const impl::pipeline_layout_params& create) {
            if (impl.pipeline_layouts.contains(id))
            {
                return id;
            }

            uint64_t layout{};
            if (this->create_pipeline_layout(device, create.push_constant_stages, create.push_constant_size, set_layout_ids(device, create),
                                             layout) != VK_SUCCESS)
            {
                throw std::runtime_error("Re-creating a destroyed Vulkan pipeline layout failed");
            }
            temporaries.emplace_back([this, device, layout] { this->destroy_pipeline_layout(device, layout); });
            return layout;
        };

        const auto ensure_render_pass = [&](const uint64_t device, const uint64_t id, const impl::render_pass_params& create) -> uint64_t {
            if (id == 0 || impl.render_passes.contains(id))
            {
                return id;
            }

            uint64_t render_pass{};
            if (this->create_render_pass(device, create.format, create.load_op, create.store_op, create.initial_layout, create.final_layout,
                                         create.depth_format, render_pass) != VK_SUCCESS)
            {
                throw std::runtime_error("Re-creating a destroyed Vulkan render pass failed");
            }
            temporaries.emplace_back([this, device, render_pass] { this->destroy_render_pass(device, render_pass); });
            return render_pass;
        };

        // Stages whose module was destroyed get a temporary one from the saved code, also those given by an identifier.
        const auto stage_source = [&](const uint64_t device, const impl::shader_stage_params& stage) -> shader_stage_source {
            if (stage.module != 0 && impl.shader_modules.contains(stage.module))
            {
                return {.module = stage.module, .identifier = {}};
            }

            if (!stage.code)
            {
                return {.module = 0, .identifier = stage.identifier};
            }

            uint64_t module{};
            if (this->create_shader_module(device, stage.code->flags, stage.code->code.data(), stage.code->code.size(), module) !=
                VK_SUCCESS)
            {
                throw std::runtime_error("Re-creating a destroyed Vulkan shader module failed");
            }
            temporaries.emplace_back([this, device, module] { this->destroy_shader_module(device, module); });
            return {.module = module, .identifier = {}};
        };

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            std::vector<std::string> extensions(buffer.read<uint64_t>());
            for (auto& extension : extensions)
            {
                extension = buffer.read_string<char>();
            }

            uint64_t instance{};
            impl.forced_ids = {id};
            const auto result = this->create_instance(extensions, instance);
            expect_restored(result, instance, id, "instance");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto instance_id = buffer.read<uint64_t>();
            const auto index = buffer.read<uint32_t>();
            const auto vendor_id = buffer.read<uint32_t>();
            const auto device_id = buffer.read<uint32_t>();

            const auto& instance = impl.instances.at(instance_id);
            const auto native = enumerate_native_physical_devices(instance.enumerate_physical_devices, instance.handle);

            VkPhysicalDeviceProperties properties{};
            if (index < native.size() && instance.get_physical_device_properties)
            {
                instance.get_physical_device_properties(native[index], &properties);
            }

            if (index >= native.size() || properties.vendorID != vendor_id || properties.deviceID != device_id)
            {
                throw std::runtime_error("The snapshot was taken on a different GPU");
            }

            impl.physical_devices.emplace(id, impl::physical_device_data{.handle = native[index], .instance_id = instance_id});
            impl.physical_device_ids.emplace(native[index], id);
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            impl::device_create_params create{};
            buffer.read(create.physical_device);
            buffer.read_vector(create.queues);
            buffer.read_vector(create.extensions);
            buffer.read(create.extension_count);
            buffer.read_vector(create.features);
            buffer.read(create.feature_count);
            buffer.read(create.spoof_missing_features);

            uint64_t device{};
            impl.forced_ids = {id};
            const auto result =
                this->create_device(create.physical_device, create.queues.data(), create.queues.size(), create.extensions.data(),
                                    create.extensions.size(), create.extension_count, create.features.data(), create.features.size(),
                                    create.feature_count, create.spoof_missing_features, device);
            expect_restored(result, device, id, "device");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto queue_family_index = buffer.read<uint32_t>();
            const auto queue_index = buffer.read<uint32_t>();

            uint64_t queue{};
            impl.forced_ids = {id};
            const auto result = this->get_device_queue(device, queue_family_index, queue_index, queue);
            expect_restored(result, queue, id, "queue");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto size = buffer.read<uint64_t>();
            const auto memory_type_index = buffer.read<uint32_t>();
            const auto flags = buffer.read<uint32_t>();
            const auto device_mask = buffer.read<uint32_t>();
            const auto imported = buffer.read<bool>();

            uint64_t memory{};
            impl.forced_ids = {id};
            if (imported)
            {
                const auto result = this->import_host_memory(device, size, memory_type_index, flags, device_mask,
                                                             callbacks.imported_host_pointer(id), memory);
                expect_restored(result, memory, id, "imported memory");
                continue;
            }

            const auto result = this->allocate_memory(device, size, memory_type_index, flags, device_mask, memory);
            expect_restored(result, memory, id, "memory");

            auto contents = buffer.read_vector<std::byte>();
            const auto& dev = impl.devices.at(device);
            const auto& mem = impl.memories.at(id);
            if (contents.size() != mem.allocation_size)
            {
                throw std::runtime_error("The snapshot holds the wrong size for Vulkan memory " + std::to_string(id));
            }

            const auto memory_type = impl.substitute_cached_memory_type(dev, memory_type_index);
            if (!this->is_host_visible_memory_type(device, memory_type_index))
            {
                if (!transfer_for(device).copy(mem.handle, memory_type, contents, true))
                {
                    throw std::runtime_error("Restoring the contents of Vulkan memory " + std::to_string(id) + " failed");
                }
                continue;
            }

            void* mapped = nullptr;
            if (!dev.map_memory || dev.map_memory(dev.handle, mem.handle, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            {
                throw std::runtime_error("Restoring the contents of Vulkan memory " + std::to_string(id) + " failed");
            }

            std::memcpy(mapped, contents.data(), contents.size());
            dev.unmap_memory(dev.handle, mem.handle);
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto size = buffer.read<uint64_t>();
            const auto usage = buffer.read<uint32_t>();
            const auto memory = buffer.read<uint64_t>();
            const auto memory_offset = buffer.read<uint64_t>();

            uint64_t buffer_object{};
            impl.forced_ids = {id};
            const auto result = this->create_buffer(device, size, usage, buffer_object);
            expect_restored(result, buffer_object, id, "buffer");

            if (memory != 0 && this->bind_buffer_memory(device, id, memory, memory_offset) != VK_SUCCESS)
            {
                throw std::runtime_error("Binding restored Vulkan buffer " + std::to_string(id) + " failed");
            }
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::image_create_params>();
            const auto memory = buffer.read<uint64_t>();
            const auto memory_offset = buffer.read<uint64_t>();
            auto layouts = buffer.read_vector<uint32_t>();

            uint64_t image{};
            impl.forced_ids = {id};
            const auto result =
                this->create_image(device, create.format, create.width, create.height, create.usage, create.tiling, create.samples,
                                   create.image_type, create.depth, create.mip_levels, create.array_layers, create.flags, image);
            expect_restored(result, image, id, "image");

            auto& restored = impl.images.at(id);
            if (layouts.size() != restored.layouts.size() ||
                (memory != 0 && this->bind_image_memory(device, id, memory, memory_offset) != VK_SUCCESS))
            {
                throw std::runtime_error("Restoring Vulkan image " + std::to_string(id) + " failed");
            }

            struct saved_subresource
            {
                uint32_t aspect{};
                uint32_t mip{};
                uint32_t layer{};
                std::vector<std::byte> contents{};
            };

            std::vector<saved_subresource> saved{};
            while (buffer.read<bool>())
            {
                auto& entry = saved.emplace_back();
                buffer.read(entry.aspect);
                buffer.read(entry.mip);
                buffer.read(entry.layer);
                buffer.read_vector(entry.contents);
            }

            if (!saved.empty() && !restored.copyable)
            {
                throw std::runtime_error("Vulkan image " + std::to_string(id) + " can no longer be copied on this GPU");
            }

            const auto aspects = get_vulkan_format_aspects(create.format);
            for (auto& entry : saved)
            {
                const bool first = std::ranges::none_of(saved, [&](const saved_subresource& other) {
                    return &other < &entry && other.mip == entry.mip && other.layer == entry.layer;
                });
                const bool last = std::ranges::none_of(saved, [&](const saved_subresource& other) {
                    return &other > &entry && other.mip == entry.mip && other.layer == entry.layer;
                });

                const auto block = get_vulkan_copy_block(create.format, entry.aspect);
                const VkExtent3D extent{.width = std::max(1U, create.width >> entry.mip),
                                        .height = std::max(1U, create.height >> entry.mip),
                                        .depth = std::max(1U, create.depth >> entry.mip)};
                if (!block || entry.mip >= create.mip_levels || entry.layer >= create.array_layers ||
                    !transfer_for(device).copy_image(
                        restored.handle, static_cast<VkImageAspectFlagBits>(entry.aspect), aspects, entry.mip, entry.layer, extent, *block,
                        first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        static_cast<VkImageLayout>(layouts[restored.subresource_index(0, entry.mip, entry.layer)]), last, entry.contents,
                        true))
                {
                    throw std::runtime_error("Restoring the contents of Vulkan image " + std::to_string(id) + " failed");
                }
            }

            restored.layouts = std::move(layouts);
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto instance = buffer.read<uint64_t>();

            uint64_t surface{};
            impl.forced_ids = {id};
            const auto result = this->create_surface(instance, callbacks.create_native_surface(id, instance), surface);
            expect_restored(result, surface, id, "surface");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto surface = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::swapchain_params>();
            const auto image_ids = buffer.read_vector<uint64_t>();

            uint64_t swapchain{};
            uint32_t image_count = 0;
            impl.forced_ids.assign(image_ids.begin(), image_ids.end());
            impl.forced_ids.push_back(id);
            const auto result = this->create_swapchain(device, surface, 0, create.flags, create.format, create.color_space, create.width,
                                                       create.height, create.image_array_layers, create.min_image_count, create.image_usage,
                                                       create.sharing_mode, create.queue_family_indices, create.pre_transform,
                                                       create.composite_alpha, create.present_mode, create.clipped, swapchain, image_count);
            if (result == VK_SUCCESS && image_count != image_ids.size())
            {
                throw std::runtime_error("The re-created Vulkan swapchain has a different number of images");
            }
            expect_restored(result, swapchain, id, "swapchain");

            for (const auto image : image_ids)
            {
                auto& restored = impl.images.at(image);
                auto layouts = buffer.read_vector<uint32_t>();
                if (layouts.size() != restored.layouts.size())
                {
                    throw std::runtime_error("The re-created Vulkan swapchain has different images");
                }
                restored.layouts = std::move(layouts);
                restored.layout_undefined = true;
            }
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto query_type = buffer.read<uint32_t>();
            const auto query_count = buffer.read<uint32_t>();
            const auto pipeline_statistics = buffer.read<uint32_t>();

            std::unordered_map<uint32_t, std::vector<uint64_t>> available{};
            for (auto results = buffer.read<uint64_t>(); results > 0; --results)
            {
                const auto query = buffer.read<uint32_t>();
                available[query] = buffer.read_vector<uint64_t>();
            }

            uint64_t pool{};
            impl.forced_ids = {id};
            const auto result = this->create_query_pool(device, query_type, query_count, pipeline_statistics, pool);
            expect_restored(result, pool, id, "query pool");

            auto& restored = impl.query_pools.at(id);
            if (query_count > 0)
            {
                transfer_for(device).reset_query_pool(restored.handle, query_count);
            }
            restored.restored_results = std::move(available);
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto desc = buffer.read<sampler_desc>();

            uint64_t sampler{};
            impl.forced_ids = {id};
            const auto result = this->create_sampler(device, desc, sampler);
            expect_restored(result, sampler, id, "sampler");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto code = buffer.read<impl::shader_code>();

            uint64_t module{};
            impl.forced_ids = {id};
            const auto result = this->create_shader_module(device, code.flags, code.code.data(), code.code.size(), module);
            expect_restored(result, module, id, "shader module");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            auto identifier = buffer.read_string<char>();
            impl.identified_code[std::move(identifier)] = std::make_shared<const impl::shader_code>(buffer.read<impl::shader_code>());
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::set_layout_params>();

            uint64_t layout{};
            impl.forced_ids = {id};
            const auto result = this->create_descriptor_set_layout(device, create.flags, create.bindings, layout);
            expect_restored(result, layout, id, "descriptor set layout");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::pipeline_layout_params>();

            const auto set_layouts = set_layout_ids(device, create);
            uint64_t layout{};
            impl.forced_ids = {id};
            const auto result =
                this->create_pipeline_layout(device, create.push_constant_stages, create.push_constant_size, set_layouts, layout);
            expect_restored(result, layout, id, "pipeline layout");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::render_pass_params>();

            uint64_t render_pass{};
            impl.forced_ids = {id};
            const auto result = this->create_render_pass(device, create.format, create.load_op, create.store_op, create.initial_layout,
                                                         create.final_layout, create.depth_format, render_pass);
            expect_restored(result, render_pass, id, "render pass");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto flags = buffer.read<uint32_t>();
            const auto data = buffer.read_vector<uint8_t>();

            uint64_t cache{};
            impl.forced_ids = {id};
            const auto result = this->create_pipeline_cache(device, flags, data, cache);
            expect_restored(result, cache, id, "pipeline cache");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::pipeline_params>();

            const auto cache = impl.pipeline_caches.contains(create.pipeline_cache) ? create.pipeline_cache : 0;
            const auto layout = ensure_pipeline_layout(device, create.pipeline_layout, *create.layout_create);
            const auto vertex = stage_source(device, create.vertex);

            uint64_t pipeline{};
            int32_t result{};
            if (create.compute)
            {
                impl.forced_ids = {id};
                result = this->create_compute_pipeline(device, cache, layout, vertex, create.flags, pipeline);
            }
            else
            {
                const auto render_pass = ensure_render_pass(device, create.render_pass, create.render_pass_create);
                const auto fragment = stage_source(device, create.fragment);
                impl.forced_ids = {id};
                result = this->create_graphics_pipeline(
                    device, cache, render_pass, layout, vertex, fragment, create.flags, create.width, create.height, create.bindings,
                    create.attributes, create.divisors, create.depth, create.color_formats, create.depth_format, create.stencil_format,
                    create.rasterization_samples, create.primitive_topology, create.primitive_restart_enable, create.rasterization_stream,
                    create.rasterization_stream_flags, create.dynamic_states,
                    specialization{.entries = create.vs_spec.entries, .data = create.vs_spec.data},
                    specialization{.entries = create.fs_spec.entries, .data = create.fs_spec.data}, create.blend_attachments, pipeline);
            }
            expect_restored(result, pipeline, id, "pipeline");
        }

        // Views and framebuffers whose image, buffer or attachments are gone cannot be used any more, so they are not
        // re-created.
        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::image_view_params>();
            if (!impl.images.contains(create.image))
            {
                continue;
            }

            uint64_t view{};
            impl.forced_ids = {id};
            const auto result =
                this->create_image_view(device, create.image, create.format, create.aspect_mask, create.view_type, create.base_mip_level,
                                        create.level_count, create.base_array_layer, create.layer_count, create.swizzle[0],
                                        create.swizzle[1], create.swizzle[2], create.swizzle[3], view);
            expect_restored(result, view, id, "image view");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::buffer_view_params>();
            if (!impl.buffers.contains(create.buffer))
            {
                continue;
            }

            uint64_t view{};
            impl.forced_ids = {id};
            const auto result = this->create_buffer_view(device, create.buffer, create.format, create.offset, create.range, view);
            expect_restored(result, view, id, "buffer view");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::framebuffer_params>();
            if (!impl.image_views.contains(create.image_view) || (create.depth_view != 0 && !impl.image_views.contains(create.depth_view)))
            {
                continue;
            }

            const auto render_pass = ensure_render_pass(device, create.render_pass, create.render_pass_create);
            uint64_t framebuffer{};
            impl.forced_ids = {id};
            const auto result = this->create_framebuffer(device, render_pass, create.image_view, create.depth_view, create.width,
                                                         create.height, framebuffer);
            expect_restored(result, framebuffer, id, "framebuffer");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::descriptor_pool_params>();

            uint64_t pool{};
            impl.forced_ids = {id};
            const auto result = this->create_descriptor_pool(device, create.max_sets, create.flags,
                                                             create.max_inline_uniform_block_bindings, create.sizes, pool);
            expect_restored(result, pool, id, "descriptor pool");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto pool = buffer.read<uint64_t>();
            const auto layout_id = buffer.read<uint64_t>();
            const auto layout_create = buffer.read<impl::set_layout_params>();
            const auto has_variable_count = buffer.read<bool>();
            const auto variable_count = buffer.read<uint32_t>();

            std::vector<std::pair<uint64_t, impl::descriptor_slot>> slots(buffer.read<uint64_t>());
            for (auto& [key, slot] : slots)
            {
                buffer.read(key);
                buffer.read(slot);
            }

            std::vector<std::pair<uint32_t, std::vector<std::byte>>> inline_blocks(buffer.read<uint64_t>());
            for (auto& [binding, block] : inline_blocks)
            {
                buffer.read(binding);
                buffer.read_vector(block);
            }

            const std::array layouts{ensure_set_layout(device, layout_id, layout_create)};
            const std::array counts{variable_count};
            std::array<uint64_t, 1> sets{};
            uint32_t allocated = 0;
            impl.forced_ids = {id};
            const auto result = this->allocate_descriptor_sets(
                device, pool, layouts, has_variable_count ? std::span<const uint32_t>{counts} : std::span<const uint32_t>{}, sets,
                allocated);
            expect_restored(result, allocated == 1 ? sets[0] : 0, id, "descriptor set");

            auto& restored = impl.descriptor_sets.at(id);
            restored.layout_id = layout_id;

            std::vector<descriptor_write> writes{};
            for (const auto& [key, slot] : slots)
            {
                writes.push_back(descriptor_write{.dst_set = id,
                                                  .dst_binding = static_cast<uint32_t>(key >> 32),
                                                  .dst_array_element = static_cast<uint32_t>(key),
                                                  .descriptor_type = slot.descriptor_type,
                                                  .buffer_or_view = slot.buffer_or_view,
                                                  .offset = slot.offset,
                                                  .range = slot.range,
                                                  .sampler = slot.sampler,
                                                  .image_view = slot.image_view,
                                                  .image_layout = slot.image_layout,
                                                  .inline_uniform_data = {}});
            }

            for (const auto& [binding, block] : inline_blocks)
            {
                writes.push_back(descriptor_write{.dst_set = id,
                                                  .dst_binding = binding,
                                                  .dst_array_element = 0,
                                                  .descriptor_type = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
                                                  .buffer_or_view = 0,
                                                  .offset = 0,
                                                  .range = 0,
                                                  .sampler = 0,
                                                  .image_view = 0,
                                                  .image_layout = 0,
                                                  .inline_uniform_data = block});
            }

            // A descriptor referring to an object destroyed since is invalid and cannot be written again; the guest may
            // not use it.
            if (this->update_descriptor_sets(device, writes, {}) != VK_SUCCESS)
            {
                for (const auto& write : writes)
                {
                    (void)this->update_descriptor_sets(device, std::span{&write, 1}, {});
                }
            }
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto flags = buffer.read<uint32_t>();
            const auto set = buffer.read<bool>();

            uint64_t event{};
            impl.forced_ids = {id};
            const auto result = this->create_event(device, flags, event);
            expect_restored(result, event, id, "event");
            if (set && this->set_event(device, id) != VK_SUCCESS)
            {
                throw std::runtime_error("Restoring Vulkan event " + std::to_string(id) + " failed");
            }
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto queue_family_index = buffer.read<uint32_t>();
            const auto flags = buffer.read<uint32_t>();

            uint64_t pool{};
            impl.forced_ids = {id};
            const auto result = this->create_command_pool(device, queue_family_index, flags, pool);
            expect_restored(result, pool, id, "command pool");
        }

        std::vector<uint64_t> pending_binary_signals{};

        struct recorded_commands
        {
            uint64_t command_buffer{};
            std::vector<std::byte> commands{};
            std::vector<int32_t> results{};
            bool invalidated{};
        };

        std::vector<recorded_commands> recorded_primaries{};
        std::vector<recorded_commands> recorded_secondaries{};

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto pool = buffer.read<uint64_t>();
            const auto level = buffer.read<uint32_t>();
            recorded_commands recorded{.command_buffer = id};
            buffer.read_vector(recorded.commands);
            buffer.read_vector(recorded.results);
            buffer.read(recorded.invalidated);
            if (!recorded.commands.empty())
            {
                (level == 1 ? recorded_secondaries : recorded_primaries).push_back(std::move(recorded));
            }

            uint64_t command_buffer{};
            impl.forced_ids = {id};
            const auto result = this->allocate_command_buffer(device, pool, level, command_buffer);
            expect_restored(result, command_buffer, id, "command buffer");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto signaled = buffer.read<bool>();

            uint64_t fence{};
            impl.forced_ids = {id};
            const auto result = this->create_fence(device, signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0, fence);
            expect_restored(result, fence, id, "fence");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto flags = buffer.read<uint32_t>();
            const auto type = buffer.read<uint32_t>();
            const auto value = buffer.read<uint64_t>();
            const auto binary_signaled = buffer.read<bool>();
            const auto binary_wait_unsignaled = buffer.read<bool>();
            const auto highest_signal = buffer.read<uint64_t>();
            const auto highest_wait = buffer.read<uint64_t>();

            uint64_t semaphore{};
            impl.forced_ids = {id};
            const auto result = this->create_semaphore(device, flags, type, value, semaphore);
            expect_restored(result, semaphore, id, "semaphore");

            auto& restored = impl.semaphores.at(id);
            restored.binary_wait_unsignaled = binary_wait_unsignaled;
            restored.highest_signal = highest_signal;
            restored.highest_wait = highest_wait;
            if (binary_signaled)
            {
                pending_binary_signals.push_back(id);
            }
        }

        for (const auto id : pending_binary_signals)
        {
            const auto& semaphore = impl.semaphores.at(id);
            const auto queue =
                std::ranges::find_if(impl.queues, [&](const auto& entry) { return entry.second.device_id == semaphore.device_id; });
            const gpu_bridge::submit_semaphore_entry signal{.semaphore = id, .stage_mask = 0, .reserved = 0, .value = 0};
            if (queue == impl.queues.end() ||
                this->queue_submit(queue->first, 0, nullptr, 0, nullptr, 0, &signal, 1, false) != VK_SUCCESS ||
                this->queue_wait_idle(queue->first) != VK_SUCCESS)
            {
                throw std::runtime_error("Restoring the signal of Vulkan semaphore " + std::to_string(id) + " failed");
            }
        }

        for (const auto* recorded : {&recorded_secondaries, &recorded_primaries})
        {
            for (const auto& entry : *recorded)
            {
                callbacks.replay_commands(entry.command_buffer, entry.commands, entry.results, entry.invalidated);
            }
        }
    }
}
