#include "vulkan_host.hpp"
#include "../vulkan_handle_utils.hpp"
#include "spirv_clip_distance.hpp"
#include "vulkan_format_block.hpp"
#include "snapshot_shaders.hpp"

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
#include <tuple>
#include <map>
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

        // Unused on 32-bit hosts, which load no Vulkan driver.
        [[maybe_unused]] library_handle load_library(const char* name)
        {
            return ::LoadLibraryA(name);
        }

        [[maybe_unused]] void* get_symbol(library_handle handle, const char* name)
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

        // Unused on 32-bit hosts, which load no Vulkan driver.
        [[maybe_unused]] library_handle load_library(const char* name)
        {
            return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        }

        [[maybe_unused]] void* get_symbol(library_handle handle, const char* name)
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

        bool loaded{};
        library_handle loader{};
        PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
        PFN_vkCreateInstance create_instance{};
        PFN_vkEnumerateInstanceVersion enumerate_instance_version{};

        struct instance_data
        {
            VkInstance handle{};
            // Whether the instance extensions present fences need are enabled, which the guest never asked for.
            bool surface_maintenance1{};
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

        // The memory types a guest sees for a physical device. On devices where all memory is host visible (unified memory,
        // e.g. Mali), a 32-bit guest imports every host-visible allocation into its own address space, including the
        // images and buffers it never maps, and runs out of it. There, each device-local, host-visible type stays at its
        // index as device-local only, and its host-visible form is appended; first-match device-local picks then take the
        // former. Both forms are backed by the same host type.
        struct guest_memory_types
        {
            VkPhysicalDeviceMemoryProperties properties{};
            std::array<uint32_t, VK_MAX_MEMORY_TYPES> host_index{};
            std::array<uint32_t, VK_MAX_MEMORY_TYPES> host_visible_index{};

            // An index outside the guest's types is passed on unchanged, for the caller's own range check.
            uint32_t to_host(const uint32_t guest_index) const
            {
                return guest_index < this->properties.memoryTypeCount ? this->host_index[guest_index] : guest_index;
            }

            uint32_t to_guest_bits(const uint32_t host_bits) const
            {
                uint32_t guest_bits = host_bits;
                for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; ++i)
                {
                    if ((host_bits & (1u << i)) != 0 && this->host_visible_index[i] != UINT32_MAX)
                    {
                        guest_bits |= 1u << this->host_visible_index[i];
                    }
                }
                return guest_bits;
            }

            bool is_host_visible(const uint32_t guest_index) const
            {
                return guest_index < this->properties.memoryTypeCount &&
                       (this->properties.memoryTypes[guest_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
            }
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
            guest_memory_types memory_types{};
            uint32_t queue_family_index{}; // the single family this device was created with
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
            PFN_vkWaitForFences wait_for_fences{};
            bool present_fences{};
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
            PFN_vkCreateRenderPass2 create_render_pass2{};
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
            PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set{};
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
            PFN_vkCmdDispatchBase cmd_dispatch_base{};
            PFN_vkGetRenderAreaGranularity get_render_area_granularity{};
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
            PFN_vkCmdSetAlphaToCoverageEnableEXT cmd_set_alpha_to_coverage_enable{};
            PFN_vkCmdSetRasterizationSamplesEXT cmd_set_rasterization_samples{};
            PFN_vkCmdSetLineRasterizationModeEXT cmd_set_line_rasterization_mode{};
            PFN_vkCmdSetSampleMaskEXT cmd_set_sample_mask{};
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

        // A host surface may not take what a guest made for Windows asks for, such as Zink's B8G8R8A8 swapchains on
        // Android. The guest then renders into these images, and each present copies one into an image of the native
        // swapchain.
        struct swapchain_proxy
        {
            std::vector<VkImage> images{};
            std::vector<VkDeviceMemory> memories{};

            struct copy_commands
            {
                VkCommandPool pool{};
                std::vector<VkCommandBuffer> buffers{};
            };

            // Per queue family the guest presents from, created at its first present there.
            std::map<uint32_t, copy_commands> copies{};
            // Signaled once the copy out of the image is done, which the guest acquiring it again waits for. Only a copy
            // that was submitted signals it.
            std::vector<VkFence> copied{};
            std::vector<bool> copy_submitted{};
            // Per proxy image rather than per native one, since the copy waiting for it is fenced like the proxy image.
            std::vector<VkSemaphore> native_acquired{};
            // Per native image, waited for by its present.
            std::vector<VkSemaphore> native_ready{};
            std::vector<VkImage> native_images{};
            uint32_t next_image{};
            // Presents from different queues acquire native images without the kernel lock, while the swapchain
            // needs external synchronization.
            std::shared_ptr<std::mutex> native_access = std::make_shared<std::mutex>();
            // A present that failed after acquiring a native image leaves it acquired and its semaphore pending, so the
            // guest has to re-create the swapchain.
            bool lost{};
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
            // Returning from a present does not mean the presentation engine is done with the swapchain, which has to
            // outlive what it still presents. Each present therefore signals a fence when it is.
            std::vector<VkFence> presenting{};
            std::vector<VkFence> spare_present_fences{};
            std::optional<swapchain_proxy> proxy{};
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
            uint32_t source_family{VK_QUEUE_FAMILY_IGNORED};
            uint32_t destination_family{VK_QUEUE_FAMILY_IGNORED};
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
            // For each reset, the number of layout transitions recorded before it, which places it among the secondaries
            // the command buffer executes.
            std::vector<size_t> query_reset_positions{};
            // Saved query results this command buffer writes into buffers. Once one of the queries is reset, the command
            // buffer is recorded again before it is submitted.
            std::vector<std::pair<uint64_t, uint32_t>> restored_query_reads{};
            // A command buffer the guest may submit while it is still pending cannot be reset to record it again.
            bool simultaneous_use{};
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
            // The signals and the value the modelled execution order has reached.
            uint32_t modeled_signals{};
            uint64_t modeled_value{};
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

        // Resets that work ordered before a submission makes, which only take effect on the saved query results once that
        // work runs. While the submission is checked and recorded again, a restored query they reset counts as reset.
        std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> resets_ahead{};

        bool reset_ahead(const uint64_t query_pool, const uint32_t query) const
        {
            return std::ranges::any_of(this->resets_ahead, [&](const auto& reset) {
                const auto& [pool, first, count] = reset;
                return pool == query_pool && query >= first && query - first < count;
            });
        }

        bool depends_on_changed_state(const command_buffer_data& command_buffer) const
        {
            return std::ranges::any_of(command_buffer.restored_query_reads, [&](const auto& read) {
                const auto pool = this->query_pools.find(read.first);
                return pool == this->query_pools.end() || !pool->second.restored_results.contains(read.second) ||
                       this->reset_ahead(read.first, read.second);
            });
        }

        static std::vector<uint64_t> executed_secondaries(const command_buffer_data& command_buffer)
        {
            std::vector<uint64_t> secondaries{};
            for (const auto& transition : command_buffer.layout_transitions)
            {
                if (transition.secondary != 0)
                {
                    secondaries.push_back(transition.secondary);
                }
            }
            return secondaries;
        }

        // Adds the resets of the modelled submissions that run before a submission to the given queue with these waits: those
        // pending on the same queue, and those whose signals it waits for, together with what runs before them.
        void add_resets_ahead_of(const VkQueue queue, std::vector<std::pair<uint64_t, uint64_t>> waits)
        {
            std::unordered_set<const pending_submission*> included{};
            const auto include_up_to = [&](const std::deque<pending_submission>& pending, const size_t last) {
                for (size_t i = 0; i <= last; ++i)
                {
                    if (included.insert(&pending[i]).second)
                    {
                        this->resets_ahead.insert(this->resets_ahead.end(), pending[i].query_resets.begin(), pending[i].query_resets.end());
                        waits.insert(waits.end(), pending[i].waits.begin(), pending[i].waits.end());
                    }
                }
            };

            if (const auto same_queue = this->pending_submissions.find(queue);
                same_queue != this->pending_submissions.end() && !same_queue->second.empty())
            {
                include_up_to(same_queue->second, same_queue->second.size() - 1);
            }

            while (!waits.empty())
            {
                const auto [semaphore, value] = waits.back();
                waits.pop_back();
                const auto waited = this->semaphores.find(semaphore);
                if (waited == this->semaphores.end())
                {
                    continue;
                }

                const bool timeline = waited->second.type == VK_SEMAPHORE_TYPE_TIMELINE;
                if (timeline ? waited->second.modeled_value >= value : waited->second.modeled_signals > 0)
                {
                    continue;
                }

                // Only the signal the wait is satisfied by first runs before it: the lowest pending value that reaches it.
                const std::deque<pending_submission>* signaling_queue = nullptr;
                size_t signaler = 0;
                uint64_t signaled_value = UINT64_MAX;
                for (const auto& pending : this->pending_submissions | std::views::values)
                {
                    for (size_t i = 0; i < pending.size(); ++i)
                    {
                        for (const auto& [signaled, signal_value] : pending[i].signals)
                        {
                            const uint64_t reached = timeline ? signal_value : 0;
                            if (signaled == semaphore && (!timeline || signal_value >= value) &&
                                (!signaling_queue || reached < signaled_value))
                            {
                                signaling_queue = &pending;
                                signaler = i;
                                signaled_value = reached;
                            }
                        }
                    }
                }

                if (signaling_queue)
                {
                    include_up_to(*signaling_queue, signaler);
                }
            }
        }

        // A command buffer whose recording baked a saved query result in is recorded again before it is submitted once
        // the query was reset, including by work that runs before it in the same submission.
        bool rerecord_changed(const VkQueue queue, const std::vector<std::pair<uint64_t, uint64_t>>& waits, const uint64_t* submitted,
                              const uint32_t count, const std::function<bool(uint64_t command_buffer)>& rerecord)
        {
            const auto rerecord_one = [&](const uint64_t command_buffer) { return rerecord && rerecord(command_buffer); };

            this->add_resets_ahead_of(queue, waits);
            const auto forget_resets_ahead = utils::finally([&] { this->resets_ahead.clear(); });

            for (uint32_t i = 0; i < count; ++i)
            {
                if (i > 0)
                {
                    pending_submission preceding{};
                    this->collect_effects(submitted[i - 1], preceding);
                    this->resets_ahead.insert(this->resets_ahead.end(), preceding.query_resets.begin(), preceding.query_resets.end());
                }

                const auto cb = this->command_buffers.find(submitted[i]);
                if (cb == this->command_buffers.end())
                {
                    continue;
                }

                bool stale = this->depends_on_changed_state(cb->second);
                const auto outer_resets = this->resets_ahead.size();
                size_t next_reset = 0;
                const auto& transitions = cb->second.layout_transitions;
                for (size_t position = 0; position < transitions.size(); ++position)
                {
                    const auto& own = cb->second;
                    for (; next_reset < own.query_reset_positions.size() && own.query_reset_positions[next_reset] <= position; ++next_reset)
                    {
                        this->resets_ahead.push_back(own.query_resets[next_reset]);
                    }

                    const uint64_t secondary = transitions[position].secondary;
                    const auto executed = this->command_buffers.find(secondary);
                    if (secondary == 0 || executed == this->command_buffers.end())
                    {
                        continue;
                    }

                    if (this->depends_on_changed_state(executed->second))
                    {
                        if (!rerecord_one(secondary))
                        {
                            return false;
                        }

                        // Recording a secondary again invalidates every primary that executes it. The others are not part of
                        // this submission, so what runs before it does not apply to them.
                        std::vector<uint64_t> invalidated{};
                        for (const auto& [id, other] : this->command_buffers)
                        {
                            if (id != submitted[i] &&
                                std::ranges::any_of(other.layout_transitions,
                                                    [&](const layout_transition& transition) { return transition.secondary == secondary; }))
                            {
                                invalidated.push_back(id);
                            }
                        }

                        auto ahead = std::exchange(this->resets_ahead, {});
                        const bool recorded = std::ranges::all_of(invalidated, rerecord_one);
                        this->resets_ahead = std::move(ahead);
                        if (!recorded)
                        {
                            return false;
                        }
                        stale = true;
                    }

                    pending_submission executed_effects{};
                    this->collect_effects(secondary, executed_effects);
                    this->resets_ahead.insert(this->resets_ahead.end(), executed_effects.query_resets.begin(),
                                              executed_effects.query_resets.end());
                }
                this->resets_ahead.resize(outer_resets);

                if (stale && !rerecord_one(submitted[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Submissions to different queues run in the order their semaphores impose, not in the order they were made in,
        // and a valid guest orders transitions of the same image that way. What a submission does to images and queries is
        // therefore kept until everything it waits for is signaled by what ran before it.
        struct pending_submission
        {
            uint64_t queue_id{};
            std::vector<layout_transition> transitions{};
            std::vector<std::tuple<uint64_t, uint32_t, uint32_t>> query_resets{};
            std::vector<std::pair<uint64_t, uint64_t>> waits{};
            std::vector<std::pair<uint64_t, uint64_t>> signals{};
        };

        // A present does not delay later submissions to its queue, so presents wait in an order of their own.
        std::unordered_map<VkQueue, std::deque<pending_submission>> pending_submissions{};
        std::unordered_map<VkQueue, std::deque<pending_submission>> pending_presents{};
        // Whether all submitted work finished and nothing was submitted since.
        bool quiesced{};

        void collect_effects(const uint64_t command_buffer, pending_submission& submission, const uint32_t depth = 0)
        {
            const auto cb = this->command_buffers.find(command_buffer);
            if (cb == this->command_buffers.end() || depth > 1)
            {
                return;
            }

            submission.query_resets.insert(submission.query_resets.end(), cb->second.query_resets.begin(), cb->second.query_resets.end());
            for (const auto& transition : cb->second.layout_transitions)
            {
                if (transition.secondary != 0)
                {
                    this->collect_effects(transition.secondary, submission, depth + 1);
                }
                else
                {
                    submission.transitions.push_back(transition);
                }
            }
        }

        template <typename Entry>
        void submit_modeled(const VkQueue queue, const uint64_t queue_id, const uint64_t* command_buffers, const uint32_t count,
                            const Entry* waits, const uint32_t wait_count, const Entry* signals, const uint32_t signal_count)
        {
            pending_submission submission{.queue_id = queue_id};
            for (uint32_t i = 0; i < count; ++i)
            {
                this->collect_effects(command_buffers[i], submission);
            }
            for (uint32_t i = 0; i < wait_count; ++i)
            {
                submission.waits.emplace_back(waits[i].semaphore, waits[i].value);
            }
            for (uint32_t i = 0; i < signal_count; ++i)
            {
                submission.signals.emplace_back(signals[i].semaphore, signals[i].value);
            }

            this->pending_submissions[queue].push_back(std::move(submission));
            this->quiesced = false;
            this->run_modeled();
        }

        bool can_run(const pending_submission& submission) const
        {
            return std::ranges::all_of(submission.waits, [&](const auto& wait) {
                const auto semaphore = this->semaphores.find(wait.first);
                return semaphore == this->semaphores.end() ||
                       (semaphore->second.type == VK_SEMAPHORE_TYPE_TIMELINE ? semaphore->second.modeled_value >= wait.second
                                                                             : semaphore->second.modeled_signals > 0);
            });
        }

        void run_modeled()
        {
            for (bool progress = true; progress;)
            {
                progress = false;
                for (auto* pending : {&this->pending_submissions, &this->pending_presents})
                {
                    for (auto& queue : *pending | std::views::values)
                    {
                        while (!queue.empty() && this->can_run(queue.front()))
                        {
                            this->run_modeled(queue.front());
                            queue.pop_front();
                            progress = true;
                        }
                    }
                }
            }
        }

        void run_modeled(const pending_submission& submission)
        {
            for (const auto& [semaphore, value] : submission.waits)
            {
                if (const auto it = this->semaphores.find(semaphore);
                    it != this->semaphores.end() && it->second.type != VK_SEMAPHORE_TYPE_TIMELINE)
                {
                    --it->second.modeled_signals;
                }
            }

            for (const auto& [pool, first_query, count] : submission.query_resets)
            {
                if (const auto reset = this->query_pools.find(pool); reset != this->query_pools.end())
                {
                    reset->second.forget_restored(first_query, count);
                }
            }

            for (const auto& transition : submission.transitions)
            {
                this->apply_layout_transition(submission.queue_id, transition);
            }

            for (const auto& [semaphore, value] : submission.signals)
            {
                if (const auto it = this->semaphores.find(semaphore); it != this->semaphores.end())
                {
                    if (it->second.type == VK_SEMAPHORE_TYPE_TIMELINE)
                    {
                        it->second.modeled_value = std::max(it->second.modeled_value, value);
                    }
                    else
                    {
                        ++it->second.modeled_signals;
                    }
                }
            }
        }

        bool has_pending_submissions() const
        {
            const auto any_pending = [](const auto& pending) {
                return std::ranges::any_of(pending | std::views::values, [](const auto& queue) { return !queue.empty(); });
            };
            return any_pending(this->pending_submissions) || any_pending(this->pending_presents);
        }

        void apply_layout_transition(const uint64_t queue_id, const layout_transition& transition)
        {
            const auto image = this->images.find(transition.image);
            if (image == this->images.end())
            {
                return;
            }

            auto& data = image->second;
            data.last_queue_id = queue_id;
            const auto queue = this->queues.find(queue_id);
            const auto family = queue == this->queues.end() ? VK_QUEUE_FAMILY_IGNORED : queue->second.queue_family_index;

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
                            const auto index = data.subresource_index(aspect, mip, layer);
                            data.layouts[index] = transition.new_layout;
                            data.transfer_ownership(index, transition, family);
                        }
                    }
                }

                ++aspect;
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
                       dev.physical_device, info.format, info.imageType, info.tiling, info.usage, info.flags, &properties) == VK_SUCCESS &&
                   (properties.sampleCounts & info.samples) != 0;
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
                    it->second.binary_signaled = it->second.type != VK_SEMAPHORE_TYPE_TIMELINE;
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
            // Per subresource, the queue family that owns it and the family a release handed it to that has not acquired
            // it yet. An exclusive image belongs to the family that first used it until a release and acquire move it.
            std::vector<uint32_t> owners{};
            std::vector<uint32_t> released_to{};
            bool ownership_unknown{};
            uint64_t last_queue_id{};
            // A re-created swapchain image is undefined, while the guest believes it is in its saved layout. It is
            // transitioned there when the guest first acquires it, before the guest can use it.
            bool layout_undefined{};
            // A swapchain proxy image is never presented itself, and only presentable images may be in
            // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR. It is in VK_IMAGE_LAYOUT_GENERAL whenever the guest believes it is in that layout.
            bool swapchain_proxy{};

            VkImageLayout host_layout(const uint32_t layout) const
            {
                return this->swapchain_proxy && layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_GENERAL
                                                                                          : static_cast<VkImageLayout>(layout);
            }

            uint32_t aspect_count() const
            {
                return static_cast<uint32_t>(std::popcount(get_vulkan_format_aspects(this->create.format)));
            }

            size_t subresource_index(const uint32_t aspect, const uint32_t mip, const uint32_t layer) const
            {
                return (static_cast<size_t>(aspect) * this->create.mip_levels + mip) * this->create.array_layers + layer;
            }

            void transfer_ownership(const size_t index, const layout_transition& transition, const uint32_t family)
            {
                auto& owner = this->owners[index];
                auto& released = this->released_to[index];
                if (!transition.transfers_ownership)
                {
                    owner = family;
                }
                else if (family == transition.source_family)
                {
                    this->ownership_unknown |= owner != VK_QUEUE_FAMILY_IGNORED && owner != family;
                    owner = family;
                    released = transition.destination_family;
                }
                else if (family == transition.destination_family && released == family && owner == transition.source_family)
                {
                    owner = family;
                    released = VK_QUEUE_FAMILY_IGNORED;
                }
                else
                {
                    this->ownership_unknown = true;
                }
            }

            void reset_subresources(const size_t count)
            {
                this->layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
                this->owners.assign(count, VK_QUEUE_FAMILY_IGNORED);
                this->released_to.assign(count, VK_QUEUE_FAMILY_IGNORED);
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
            vulkan_host::render_pass_description description{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->description.flags);
                buffer.write_vector(this->description.attachments);
                buffer.write_vector(this->description.subpasses);
                buffer.write_vector(this->description.references);
                buffer.write_vector(this->description.dependencies);
                buffer.write_vector(this->description.preserves);
                buffer.write_vector(this->description.correlated_view_masks);
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->description.flags);
                buffer.read_vector(this->description.attachments);
                buffer.read_vector(this->description.subpasses);
                buffer.read_vector(this->description.references);
                buffer.read_vector(this->description.dependencies);
                buffer.read_vector(this->description.preserves);
                buffer.read_vector(this->description.correlated_view_masks);
            }
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
            uint32_t subpass{};
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
                buffer.write(this->subpass);
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
                buffer.read(this->subpass);
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
            uint32_t flags{};
            uint32_t width{};
            uint32_t height{};
            uint32_t layers{};
            std::vector<uint64_t> views{};
            std::vector<vulkan_host::framebuffer_attachment_image> images{};

            void serialize(utils::buffer_serializer& buffer) const
            {
                buffer.write(this->render_pass);
                buffer.write(this->render_pass_create);
                buffer.write(this->flags);
                buffer.write(this->width);
                buffer.write(this->height);
                buffer.write(this->layers);
                buffer.write_vector(this->views);
                buffer.write(static_cast<uint64_t>(this->images.size()));
                for (const auto& image : this->images)
                {
                    buffer.write(image.flags);
                    buffer.write(image.usage);
                    buffer.write(image.width);
                    buffer.write(image.height);
                    buffer.write(image.layer_count);
                    buffer.write_vector(image.view_formats);
                }
            }

            void deserialize(utils::buffer_deserializer& buffer)
            {
                buffer.read(this->render_pass);
                buffer.read(this->render_pass_create);
                buffer.read(this->flags);
                buffer.read(this->width);
                buffer.read(this->height);
                buffer.read(this->layers);
                buffer.read_vector(this->views);
                this->images.resize(static_cast<size_t>(buffer.read<uint64_t>()));
                for (auto& image : this->images)
                {
                    buffer.read(image.flags);
                    buffer.read(image.usage);
                    buffer.read(image.width);
                    buffer.read(image.height);
                    buffer.read(image.layer_count);
                    buffer.read_vector(image.view_formats);
                }
            }
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

        // Takes the semaphore the acquire signals and signals it again once the image is in its saved layout. An acquire
        // with only a fence is waited for first, since the image is only available to the device then.
        bool transition_restored_swapchain_image(const device_data& device, image_data& image, const VkSemaphore acquired,
                                                 const VkFence acquired_fence)
        {
            if (!acquired && acquired_fence)
            {
                VkResult status = VK_NOT_READY;
                while ((status = device.get_fence_status(device.handle, acquired_fence)) == VK_NOT_READY)
                {
                    std::this_thread::yield();
                }

                if (status != VK_SUCCESS)
                {
                    return false;
                }
            }

            // The guest last used the image on this queue, so its family owns it.
            const auto queue = this->queues.find(image.last_queue_id);
            if (queue == this->queues.end())
            {
                return false;
            }

            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue->second.queue_family_index;

            VkCommandPool pool{};
            if (device.create_command_pool(device.handle, &pool_info, nullptr, &pool) != VK_SUCCESS)
            {
                return false;
            }
            const auto destroy_pool = utils::finally([&] { device.destroy_command_pool(device.handle, pool, nullptr); });

            VkCommandBufferAllocateInfo allocate_info{};
            allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate_info.commandPool = pool;
            allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate_info.commandBufferCount = 1;

            VkCommandBuffer command_buffer{};
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (device.allocate_command_buffers(device.handle, &allocate_info, &command_buffer) != VK_SUCCESS ||
                device.begin_command_buffer(command_buffer, &begin_info) != VK_SUCCESS)
            {
                return false;
            }

            for (uint32_t layer = 0; layer < image.create.array_layers; ++layer)
            {
                const auto layout = static_cast<VkImageLayout>(image.layouts[image.subresource_index(0, 0, layer)]);
                if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
                {
                    continue;
                }

                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = image.host_layout(layout);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.handle;
                barrier.subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1};
                device.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                            nullptr, 0, nullptr, 1, &barrier);
            }

            constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = acquired ? 1 : 0;
            submit.pWaitSemaphores = &acquired;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffer;
            submit.signalSemaphoreCount = acquired ? 1 : 0;
            submit.pSignalSemaphores = &acquired;

            const auto lock = this->queue_lock(queue->second.handle);
            const std::scoped_lock guard(*lock);
            if (device.end_command_buffer(command_buffer) != VK_SUCCESS ||
                device.queue_submit(queue->second.handle, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
                device.queue_wait_idle(queue->second.handle) != VK_SUCCESS)
            {
                return false;
            }

            image.layout_undefined = false;
            return true;
        }

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
                switch (this->query_type)
                {
                case VK_QUERY_TYPE_PIPELINE_STATISTICS:
                    return static_cast<uint32_t>(std::popcount(this->pipeline_statistics));
                case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT:
                    return 2;
                default:
                    return 1;
                }
            }

            bool snapshot_supported() const
            {
                return this->query_type == VK_QUERY_TYPE_OCCLUSION || this->query_type == VK_QUERY_TYPE_PIPELINE_STATISTICS ||
                       this->query_type == VK_QUERY_TYPE_TIMESTAMP || this->query_type == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT;
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
            render_pass_params create{};
            // Variants for framebuffers with swapchain proxy images, keyed by which attachments are proxies.
            std::map<std::vector<bool>, VkRenderPass> proxy_variants{};
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
        bool conserve_guest_address_space{};

        // Set while restoring a snapshot, so the object being re-created gets the id the guest already holds.
        std::deque<uint64_t> forced_ids{};

        struct descriptor_write_storage
        {
            VkDescriptorBufferInfo buffer_info{};
            VkDescriptorImageInfo image_info{};
            VkBufferView texel_buffer_view{};
            VkWriteDescriptorSetInlineUniformBlock inline_uniform_block{};
        };

        bool build_descriptor_write(const uint64_t device, const vulkan_host::descriptor_write& w, descriptor_write_storage& storage,
                                    VkWriteDescriptorSet& vw)
        {
            vw = {};
            vw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
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
                VkWriteDescriptorSetInlineUniformBlock& inline_uniform_block = storage.inline_uniform_block;
                inline_uniform_block = {};
                inline_uniform_block.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
                inline_uniform_block.dataSize = static_cast<uint32_t>(w.inline_uniform_data.size());
                inline_uniform_block.pData = w.inline_uniform_data.empty() ? nullptr : w.inline_uniform_data.data();
                vw.pNext = &inline_uniform_block;
                vw.descriptorCount = inline_uniform_block.dataSize;
            }
            else if (is_texel_buffer)
            {
                VkBufferView& buffer_view = storage.texel_buffer_view;
                buffer_view = VK_NULL_HANDLE;
                if (w.buffer_or_view != 0)
                {
                    const auto view = this->buffer_views.find(w.buffer_or_view);
                    if (view == this->buffer_views.end() || view->second.device_id != device)
                    {
                        return false;
                    }
                    buffer_view = view->second.handle;
                }
                vw.pTexelBufferView = &buffer_view;
            }
            else if (is_image)
            {
                const auto view = this->image_views.find(w.image_view);
                if (view != this->image_views.end() && view->second.device_id != device)
                {
                    return false;
                }
                VkDescriptorImageInfo& ii = storage.image_info;
                ii.sampler = VK_NULL_HANDLE;
                if (w.sampler != 0)
                {
                    const auto smp = this->samplers.find(w.sampler);
                    if (smp == this->samplers.end() || smp->second.device_id != device)
                    {
                        return false;
                    }
                    ii.sampler = smp->second.handle;
                }
                ii.imageView = (view != this->image_views.end()) ? view->second.handle : VK_NULL_HANDLE;
                ii.imageLayout = static_cast<VkImageLayout>(w.image_layout);
                vw.pImageInfo = &ii;
            }
            else
            {
                VkDescriptorBufferInfo& bi = storage.buffer_info;
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
                    const auto buf = this->buffers.find(w.buffer_or_view);
                    if (buf == this->buffers.end() || buf->second.device_id != device)
                    {
                        return false;
                    }
                    bi.buffer = buf->second.handle;
                    bi.offset = w.offset;
                    bi.range = w.range;
                }
                vw.pBufferInfo = &bi;
            }

            return true;
        }

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

        guest_memory_types get_guest_memory_types(const instance_data& instance, const VkPhysicalDevice physical_device) const
        {
            guest_memory_types types{};
            types.host_visible_index.fill(UINT32_MAX);
            if (!instance.get_physical_device_memory_properties)
            {
                return types;
            }

            instance.get_physical_device_memory_properties(physical_device, &types.properties);
            const auto host = types.properties;
            for (uint32_t i = 0; i < host.memoryTypeCount; ++i)
            {
                types.host_index[i] = i;
            }

            if (!this->conserve_guest_address_space ||
                !has_device_extension(instance, physical_device, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
            {
                return types;
            }

            constexpr VkMemoryPropertyFlags host_access =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            const auto is_split = [](const VkMemoryType& type) {
                return (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                       (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
            };

            const auto split_count = static_cast<uint32_t>(std::count_if(host.memoryTypes, host.memoryTypes + host.memoryTypeCount,
                                                                         [&](const VkMemoryType& type) { return is_split(type); }));
            if (split_count == 0 || host.memoryTypeCount + split_count > VK_MAX_MEMORY_TYPES)
            {
                return types;
            }

            auto& count = types.properties.memoryTypeCount;
            for (uint32_t i = 0; i < host.memoryTypeCount; ++i)
            {
                if (!is_split(host.memoryTypes[i]))
                {
                    continue;
                }

                types.properties.memoryTypes[i].propertyFlags &= ~host_access;
                types.properties.memoryTypes[count] = host.memoryTypes[i];
                types.host_index[count] = i;
                types.host_visible_index[i] = count;
                ++count;
            }

            return types;
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

        static VkFormat channel_swapped_format(const VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_B8G8R8A8_UNORM:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case VK_FORMAT_R8G8B8A8_UNORM:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB:
                return VK_FORMAT_B8G8R8A8_SRGB;
            default:
                return VK_FORMAT_UNDEFINED;
            }
        }

        static VkResult query_surface_formats(const instance_data& instance, const VkPhysicalDevice physical_device,
                                              const VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR>& out_formats)
        {
            uint32_t count = 0;
            VkResult result = instance.get_surface_formats(physical_device, surface, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                return result;
            }
            out_formats.resize(count);
            result = instance.get_surface_formats(physical_device, surface, &count, out_formats.data());
            out_formats.resize(count);
            return result == VK_INCOMPLETE ? VK_SUCCESS : result;
        }

        static VkResult query_surface_present_modes(const instance_data& instance, const VkPhysicalDevice physical_device,
                                                    const VkSurfaceKHR surface, std::vector<VkPresentModeKHR>& out_modes)
        {
            uint32_t count = 0;
            VkResult result = instance.get_surface_present_modes(physical_device, surface, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                return result;
            }
            out_modes.resize(count);
            result = instance.get_surface_present_modes(physical_device, surface, &count, out_modes.data());
            out_modes.resize(count);
            return result == VK_INCOMPLETE ? VK_SUCCESS : result;
        }

        const queue_data* find_device_queue(const uint64_t device_id) const
        {
            const auto queue = std::ranges::find_if(this->queues, [&](const auto& entry) { return entry.second.device_id == device_id; });
            return queue == this->queues.end() ? nullptr : &queue->second;
        }

        static void destroy_swapchain_proxy(const device_data& dev, swapchain_proxy& proxy)
        {
            // Without present fences nothing shows that a present has consumed its native_ready semaphore yet.
            if (proxy.lost || !dev.present_fences)
            {
                dev.device_wait_idle(dev.handle);
            }

            std::vector<VkFence> submitted{};
            for (size_t i = 0; i < proxy.copied.size(); ++i)
            {
                if (proxy.copy_submitted[i])
                {
                    submitted.push_back(proxy.copied[i]);
                }
            }
            if (!submitted.empty())
            {
                dev.wait_for_fences(dev.handle, static_cast<uint32_t>(submitted.size()), submitted.data(), VK_TRUE, UINT64_MAX);
            }
            for (const VkFence fence : proxy.copied)
            {
                dev.destroy_fence(dev.handle, fence, nullptr);
            }
            for (const VkSemaphore semaphore : proxy.native_acquired)
            {
                dev.destroy_semaphore(dev.handle, semaphore, nullptr);
            }
            for (const VkSemaphore semaphore : proxy.native_ready)
            {
                dev.destroy_semaphore(dev.handle, semaphore, nullptr);
            }
            for (const auto& [family, commands] : proxy.copies)
            {
                dev.destroy_command_pool(dev.handle, commands.pool, nullptr);
            }
            for (const VkImage image : proxy.images)
            {
                dev.destroy_image(dev.handle, image, nullptr);
            }
            for (const VkDeviceMemory memory : proxy.memories)
            {
                dev.free_memory(dev.handle, memory, nullptr);
            }
            proxy = {};
        }

        // Copying with a blit converts between the channel orders, while a pair of the same encoding keeps the values.
        static VkResult check_swapchain_proxy_support(const device_data& dev, const instance_data& instance, const VkFormat format,
                                                      const VkFormat native_format, const uint32_t flags,
                                                      const VkSurfaceCapabilitiesKHR& caps)
        {
            if ((flags & ~static_cast<uint32_t>(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)) != 0 ||
                (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 || !instance.get_physical_device_format_properties ||
                !instance.get_queue_family_properties || !dev.cmd_blit_image)
            {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }

            VkFormatProperties source{};
            VkFormatProperties destination{};
            instance.get_physical_device_format_properties(dev.physical_device, format, &source);
            instance.get_physical_device_format_properties(dev.physical_device, native_format, &destination);
            if ((source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 ||
                (destination.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0)
            {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            return VK_SUCCESS;
        }

        static VkImageCreateInfo proxy_image_info(const swapchain_params& create)
        {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.flags = (create.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = static_cast<VkFormat>(create.format);
            image_info.extent = {.width = create.width, .height = create.height, .depth = 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = std::max(1U, create.image_array_layers);
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage =
                static_cast<VkImageUsageFlags>(create.image_usage) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            image_info.sharingMode = static_cast<VkSharingMode>(create.sharing_mode);
            image_info.queueFamilyIndexCount = static_cast<uint32_t>(create.queue_family_indices.size());
            image_info.pQueueFamilyIndices = create.queue_family_indices.empty() ? nullptr : create.queue_family_indices.data();
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            return image_info;
        }

        void register_swapchain_images(swapchain_data& sc, const std::vector<VkImage>& guest_images)
        {
            const auto& create = sc.create;
            sc.acquired.assign(guest_images.size(), false);
            sc.image_ids.reserve(guest_images.size());
            for (VkImage image : guest_images)
            {
                const uint64_t image_id = this->allocate_id();
                image_data data{.handle = image, .device_id = sc.device_id, .samples = 1, .owned = false};
                data.create = image_create_params{.format = create.format,
                                                  .width = create.width,
                                                  .height = create.height,
                                                  .depth = 1,
                                                  .mip_levels = 1,
                                                  .array_layers = std::max(1U, create.image_array_layers),
                                                  .samples = 1,
                                                  .usage = create.image_usage,
                                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                                  .image_type = VK_IMAGE_TYPE_2D,
                                                  .flags = sc.proxy && (create.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0
                                                               ? static_cast<uint32_t>(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
                                                               : 0U};
                data.reset_subresources(data.create.array_layers);
                data.swapchain_proxy = sc.proxy.has_value();
                this->images.emplace(image_id, std::move(data));
                sc.image_ids.push_back(image_id);
            }
        }

        VkResult create_swapchain_proxy(const device_data& dev, const VkImageCreateInfo& image_info, const uint32_t image_count,
                                        const std::vector<VkImage>& native_images, swapchain_proxy& proxy)
        {
            for (uint32_t i = 0; i < image_count; ++i)
            {
                VkImage image{};
                if (const VkResult result = dev.create_image(dev.handle, &image_info, nullptr, &image); result != VK_SUCCESS)
                {
                    return result;
                }
                proxy.images.push_back(image);

                VkMemoryRequirements requirements{};
                dev.get_image_memory_requirements(dev.handle, image, &requirements);
                uint32_t type = this->find_memory_type(dev, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (type == UINT32_MAX)
                {
                    type = this->find_memory_type(dev, requirements.memoryTypeBits, 0);
                }
                if (type == UINT32_MAX)
                {
                    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                }

                VkMemoryAllocateInfo allocate_info{};
                allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocate_info.allocationSize = requirements.size;
                allocate_info.memoryTypeIndex = type;
                VkDeviceMemory memory{};
                if (const VkResult result = dev.allocate_memory(dev.handle, &allocate_info, nullptr, &memory); result != VK_SUCCESS)
                {
                    return result;
                }
                proxy.memories.push_back(memory);
                if (const VkResult result = dev.bind_image_memory(dev.handle, image, memory, 0); result != VK_SUCCESS)
                {
                    return result;
                }
            }

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            for (uint32_t i = 0; i < image_count; ++i)
            {
                VkFence fence{};
                if (const VkResult result = dev.create_fence(dev.handle, &fence_info, nullptr, &fence); result != VK_SUCCESS)
                {
                    return result;
                }
                proxy.copied.push_back(fence);
                proxy.copy_submitted.push_back(false);

                VkSemaphore semaphore{};
                if (const VkResult result = dev.create_semaphore(dev.handle, &semaphore_info, nullptr, &semaphore); result != VK_SUCCESS)
                {
                    return result;
                }
                proxy.native_acquired.push_back(semaphore);
            }
            for (size_t i = 0; i < native_images.size(); ++i)
            {
                VkSemaphore semaphore{};
                if (const VkResult result = dev.create_semaphore(dev.handle, &semaphore_info, nullptr, &semaphore); result != VK_SUCCESS)
                {
                    return result;
                }
                proxy.native_ready.push_back(semaphore);
            }

            proxy.native_images = native_images;
            return VK_SUCCESS;
        }

        // A proxy image may be handed out again once the guest presented it and the copy out of it is done.
        static std::optional<uint32_t> find_free_proxy_image(const device_data& dev, swapchain_data& sc, bool& out_copying,
                                                             VkResult& out_error)
        {
            auto& proxy = *sc.proxy;
            const auto count = static_cast<uint32_t>(proxy.images.size());
            out_copying = false;
            out_error = VK_SUCCESS;
            for (uint32_t step = 0; step < count; ++step)
            {
                const uint32_t index = (proxy.next_image + step) % count;
                if (sc.acquired[index])
                {
                    continue;
                }

                const VkResult status = proxy.copy_submitted[index] ? dev.get_fence_status(dev.handle, proxy.copied[index]) : VK_SUCCESS;
                if (status == VK_SUCCESS)
                {
                    proxy.next_image = (index + 1) % count;
                    return index;
                }
                if (status != VK_NOT_READY)
                {
                    out_error = status;
                    return std::nullopt;
                }
                out_copying = true;
            }
            return std::nullopt;
        }

        // The proxy image does not wait for the presentation engine, so the acquire signals the guest's semaphore and
        // fence right away.
        VkResult signal_proxy_acquire(const device_data& dev, const uint64_t device_id, const VkSemaphore semaphore, const VkFence fence)
        {
            if (!semaphore && !fence)
            {
                return VK_SUCCESS;
            }

            const auto* queue = this->find_device_queue(device_id);
            if (!queue)
            {
                return VK_ERROR_DEVICE_LOST;
            }

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.signalSemaphoreCount = semaphore ? 1 : 0;
            submit.pSignalSemaphores = &semaphore;

            const auto lock = this->queue_lock(queue->handle);
            const std::scoped_lock guard(*lock);
            return dev.queue_submit(queue->handle, 1, &submit, fence);
        }

        // A blit needs a graphics queue, which the guest need not present from.
        VkResult prepare_proxy_copies(const device_data& dev, swapchain_proxy& proxy, const uint32_t family)
        {
            if (proxy.copies.contains(family))
            {
                return VK_SUCCESS;
            }

            const auto instance = this->instances.find(dev.instance_id);
            if (instance == this->instances.end() || !instance->second.get_queue_family_properties)
            {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            uint32_t family_count = 0;
            instance->second.get_queue_family_properties(dev.physical_device, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            instance->second.get_queue_family_properties(dev.physical_device, &family_count, families.data());
            if (family >= family_count || (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }

            swapchain_proxy::copy_commands commands{};
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = family;
            if (const VkResult result = dev.create_command_pool(dev.handle, &pool_info, nullptr, &commands.pool); result != VK_SUCCESS)
            {
                return result;
            }

            VkCommandBufferAllocateInfo buffers_info{};
            buffers_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            buffers_info.commandPool = commands.pool;
            buffers_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffers_info.commandBufferCount = static_cast<uint32_t>(proxy.images.size());
            commands.buffers.resize(proxy.images.size());
            if (const VkResult result = dev.allocate_command_buffers(dev.handle, &buffers_info, commands.buffers.data());
                result != VK_SUCCESS)
            {
                dev.destroy_command_pool(dev.handle, commands.pool, nullptr);
                return result;
            }

            proxy.copies.emplace(family, std::move(commands));
            return VK_SUCCESS;
        }

        static void record_proxy_copy(const device_data& dev, const VkCommandBuffer command_buffer, const VkImage source,
                                      const VkImage destination, const image_create_params& extent)
        {
            const VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .baseMipLevel = 0,
                                                .levelCount = 1,
                                                .baseArrayLayer = 0,
                                                .layerCount = extent.array_layers};

            std::array<VkImageMemoryBarrier, 2> barriers{};
            for (auto& barrier : barriers)
            {
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = range;
            }

            barriers[0].image = source;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[1].image = destination;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dev.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                     nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());

            const VkImageSubresourceLayers layers{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = extent.array_layers};
            VkImageBlit region{};
            region.srcSubresource = layers;
            region.srcOffsets[1] = {.x = static_cast<int32_t>(extent.width), .y = static_cast<int32_t>(extent.height), .z = 1};
            region.dstSubresource = layers;
            region.dstOffsets[1] = region.srcOffsets[1];
            dev.cmd_blit_image(command_buffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].dstAccessMask = 0;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].dstAccessMask = 0;
            dev.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                                     nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
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

            const auto& dev = dev_it->second;
            for (const VkFence fence : sc.spare_present_fences)
            {
                dev.destroy_fence(dev.handle, fence, nullptr);
            }
            sc.spare_present_fences.clear();

            // The swapchain and its present fences must outlive the presents. When waiting for them fails for another
            // reason than a lost device, they may still be in use and are left alive.
            if (!sc.presenting.empty())
            {
                const VkResult waited =
                    dev.wait_for_fences(dev.handle, static_cast<uint32_t>(sc.presenting.size()), sc.presenting.data(), VK_TRUE, UINT64_MAX);
                if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST)
                {
                    sc.presenting.clear();
                    sc.handle = VK_NULL_HANDLE;
                    return;
                }
            }
            for (const VkFence fence : sc.presenting)
            {
                dev.destroy_fence(dev.handle, fence, nullptr);
            }
            sc.presenting.clear();

            if (sc.proxy)
            {
                destroy_swapchain_proxy(dev, *sc.proxy);
                sc.proxy.reset();
            }

            if (sc.handle && dev_it->second.destroy_swapchain)
            {
                dev_it->second.destroy_swapchain(dev_it->second.handle, sc.handle, nullptr);
            }
            sc.handle = VK_NULL_HANDLE;
        }

        static VkResult take_present_fence(const device_data& dev, swapchain_data& sc, VkFence& out_fence)
        {
            std::erase_if(sc.presenting, [&](const VkFence fence) {
                if (dev.get_fence_status(dev.handle, fence) != VK_SUCCESS || dev.reset_fences(dev.handle, 1, &fence) != VK_SUCCESS)
                {
                    return false;
                }
                sc.spare_present_fences.push_back(fence);
                return true;
            });

            if (!sc.spare_present_fences.empty())
            {
                out_fence = sc.spare_present_fences.back();
                sc.spare_present_fences.pop_back();
                return VK_SUCCESS;
            }

            VkFenceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            return dev.create_fence(dev.handle, &info, nullptr, &out_fence);
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
                    for (const auto& [proxies, variant] : rp.proxy_variants)
                    {
                        it->second.destroy_render_pass(it->second.handle, variant, nullptr);
                    }
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
            for (const VkFence fence : it->second.abandoned_drain_fences)
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

        void load(void* external_get_instance_proc_addr)
        {
            if constexpr (sizeof(size_t) != 8)
            {
                (void)external_get_instance_proc_addr;
                return;
            }
            else
            {
                if (this->loaded)
                {
                    return;
                }

                this->loaded = true;

                if (external_get_instance_proc_addr)
                {
                    this->get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(external_get_instance_proc_addr);
                }
                else
                {
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

                    this->get_instance_proc_addr =
                        reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_symbol(this->loader, "vkGetInstanceProcAddr"));
                }

                if (!this->get_instance_proc_addr)
                {
                    return;
                }

                this->create_instance = reinterpret_cast<PFN_vkCreateInstance>(this->get_instance_proc_addr(nullptr, "vkCreateInstance"));
                this->enumerate_instance_version =
                    reinterpret_cast<PFN_vkEnumerateInstanceVersion>(this->get_instance_proc_addr(nullptr, "vkEnumerateInstanceVersion"));
            }
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

    void vulkan_host::set_conserve_guest_address_space(const bool conserve)
    {
        this->impl_->conserve_guest_address_space = conserve;
    }

    void vulkan_host::load(void* get_instance_proc_addr)
    {
        this->impl_->load(get_instance_proc_addr);
    }

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

        bool surface_maintenance1 = false;
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

            const auto enabled = [&](const std::string_view name) {
                return std::ranges::any_of(extension_names, [&](const char* extension) { return name == extension; });
            };
            const auto offered = [&](const std::string_view name) {
                return std::ranges::any_of(
                    available, [&](const VkExtensionProperties& ext) { return name == static_cast<const char*>(ext.extensionName); });
            };
            constexpr std::array present_fence_extensions{VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                                                          VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
            if (api_version >= VK_API_VERSION_1_1 && enabled(VK_KHR_SURFACE_EXTENSION_NAME) &&
                std::ranges::all_of(present_fence_extensions, offered))
            {
                for (const char* name : present_fence_extensions)
                {
                    if (!enabled(name))
                    {
                        extension_names.push_back(name);
                    }
                }
                surface_maintenance1 = true;
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
        data.surface_maintenance1 = surface_maintenance1;
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

        // Guests that take the first device (Mesa's Zink, vkd3d-proton without a matching DXGI adapter) would otherwise
        // land on a hybrid machine's integrated GPU, as on Windows without a high-performance GPU preference.
        if (it->second.get_physical_device_properties)
        {
            const auto rank = [&](const VkPhysicalDevice device) {
                VkPhysicalDeviceProperties properties{};
                it->second.get_physical_device_properties(device, &properties);
                switch (properties.deviceType)
                {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return 0;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return 1;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    return 4;
                default:
                    return 3;
                }
            };
            std::ranges::stable_sort(devices, std::less{}, rank);
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
        // The host instance is created for at most Vulkan 1.3, and the shim does not implement the 1.4 core commands.
        if (properties.apiVersion > VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(properties.apiVersion)))
        {
            properties.apiVersion = VK_API_VERSION_1_3;
        }

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
            if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            {
                // Protected queues and Ycbcr conversions are not bridged.
                auto* features = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(buffer.data());
                features->protectedMemory = VK_FALSE;
                features->samplerYcbcrConversion = VK_FALSE;
            }
            else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            {
                // Opaque capture addresses are host addresses that a replay could not reproduce.
                reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(buffer.data())->bufferDeviceAddressCaptureReplay = VK_FALSE;
            }
            else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT)
            {
                auto* features = reinterpret_cast<VkPhysicalDeviceTransformFeedbackFeaturesEXT*>(buffer.data());
                // TODO: Expose geometry-stream support after geometry shaders are bridged.
                features->geometryStreams = VK_FALSE;
            }
            else if (base->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT)
            {
                // The shim only bridges these setters; any other state a driver offers would hand the guest a null
                // vkCmdSet* pointer.
                const auto* reported = reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*>(buffer.data());
                VkPhysicalDeviceExtendedDynamicState3FeaturesEXT bridged{};
                bridged.sType = reported->sType;
                bridged.pNext = reported->pNext;
                bridged.extendedDynamicState3DepthClipEnable = reported->extendedDynamicState3DepthClipEnable;
                bridged.extendedDynamicState3AlphaToCoverageEnable = reported->extendedDynamicState3AlphaToCoverageEnable;
                bridged.extendedDynamicState3RasterizationSamples = reported->extendedDynamicState3RasterizationSamples;
                bridged.extendedDynamicState3SampleMask = reported->extendedDynamicState3SampleMask;
                bridged.extendedDynamicState3LineRasterizationMode = reported->extendedDynamicState3LineRasterizationMode;
                std::memcpy(buffer.data(), &bridged, sizeof(bridged));
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

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT present_fence_features{};
        present_fence_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        bool present_fences = false;
        if (instance->second.surface_maintenance1 && instance->second.get_physical_device_features2 &&
            std::ranges::any_of(extensions, [](const char* name) { return std::strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }) &&
            impl::has_device_extension(instance->second, pd->second.handle, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
        {
            VkPhysicalDeviceFeatures2 supported{};
            supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            supported.pNext = &present_fence_features;
            instance->second.get_physical_device_features2(pd->second.handle, &supported);

            if (present_fence_features.swapchainMaintenance1)
            {
                present_fence_features.pNext = nullptr;
                feature_tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&present_fence_features);
                feature_tail = reinterpret_cast<VkBaseOutStructure*>(&present_fence_features);
                extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                present_fences = true;
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
        if (has_feature_chain || placed_map_alignment != 0 || present_fences)
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
        data.present_fences = present_fences;

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
            data.wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(resolve("vkWaitForFences"));
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
            data.create_render_pass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(resolve("vkCreateRenderPass2"));
            if (!data.create_render_pass2)
            {
                data.create_render_pass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(resolve("vkCreateRenderPass2KHR"));
            }
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
            data.cmd_push_descriptor_set = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(resolve("vkCmdPushDescriptorSetKHR"));
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
            data.cmd_dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(resolve("vkCmdDispatchBase"));
            if (!data.cmd_dispatch_base)
            {
                data.cmd_dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(resolve("vkCmdDispatchBaseKHR"));
            }
            data.get_render_area_granularity = reinterpret_cast<PFN_vkGetRenderAreaGranularity>(resolve("vkGetRenderAreaGranularity"));
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
            data.cmd_set_alpha_to_coverage_enable =
                reinterpret_cast<PFN_vkCmdSetAlphaToCoverageEnableEXT>(resolve("vkCmdSetAlphaToCoverageEnableEXT"));
            data.cmd_set_rasterization_samples =
                reinterpret_cast<PFN_vkCmdSetRasterizationSamplesEXT>(resolve("vkCmdSetRasterizationSamplesEXT"));
            data.cmd_set_line_rasterization_mode =
                reinterpret_cast<PFN_vkCmdSetLineRasterizationModeEXT>(resolve("vkCmdSetLineRasterizationModeEXT"));
            data.cmd_set_sample_mask = reinterpret_cast<PFN_vkCmdSetSampleMaskEXT>(resolve("vkCmdSetSampleMaskEXT"));
            data.cmd_set_stencil_test_enable = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(resolve("vkCmdSetStencilTestEnable"));
            data.cmd_set_rasterizer_discard_enable =
                reinterpret_cast<PFN_vkCmdSetRasterizerDiscardEnable>(resolve("vkCmdSetRasterizerDiscardEnable"));
            data.cmd_set_depth_bias_enable = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(resolve("vkCmdSetDepthBiasEnable"));
            data.cmd_set_primitive_restart_enable =
                reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(resolve("vkCmdSetPrimitiveRestartEnable"));
        }

        data.memory_types = this->impl_->get_guest_memory_types(instance->second, pd->second.handle);

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

        for (const auto& [id, data] : this->impl_->queues)
        {
            if (data.device_id == device && data.handle == queue)
            {
                out_queue = id;
                return VK_SUCCESS;
            }
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
            cb->second.simultaneous_use = (flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0;
            cb->second.recorded.clear();
            cb->second.recorded_results.clear();
            cb->second.references.clear();
            cb->second.layout_transitions.clear();
            cb->second.query_resets.clear();
            cb->second.query_reset_positions.clear();
            cb->second.restored_query_reads.clear();
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
            auto& primary = cb->second;
            primary.layout_transitions.push_back(impl::layout_transition{.secondary = id});

            // The primary's own later query copies have to see what the secondary resets.
            const auto& executed = this->impl_->command_buffers.at(id);
            for (const auto& reset : executed.query_resets)
            {
                primary.query_resets.push_back(reset);
                primary.query_reset_positions.push_back(primary.layout_transitions.size());
            }
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
                    command_buffer.query_reset_positions.clear();
                    command_buffer.restored_query_reads.clear();
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

    std::vector<std::byte> vulkan_host::recorded_commands(const uint64_t command_buffer) const
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        return cb == this->impl_->command_buffers.end() ? std::vector<std::byte>{} : cb->second.recorded;
    }

    std::vector<int32_t> vulkan_host::recorded_results(const uint64_t command_buffer) const
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        return cb == this->impl_->command_buffers.end() ? std::vector<int32_t>{} : cb->second.recorded_results;
    }

    bool vulkan_host::can_reset_command_buffer(const uint64_t command_buffer) const
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return false;
        }

        const auto pool = this->impl_->command_pools.find(cb->second.pool_id);
        return !cb->second.simultaneous_use && pool != this->impl_->command_pools.end() &&
               (pool->second.flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) != 0;
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
            cb->second.query_reset_positions.clear();
            cb->second.restored_query_reads.clear();
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
            id, impl::semaphore_data{
                    .handle = semaphore, .device_id = device, .flags = flags, .type = semaphore_type, .modeled_value = initial_value});
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
        const VkResult result = dev->second.signal_semaphore(dev->second.handle, &info);
        if (result == VK_SUCCESS)
        {
            it->second.modeled_value = std::max(it->second.modeled_value, value);
            this->impl_->run_modeled();
        }
        return result;
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
                                      uint32_t signal_count, bool has_timeline_info,
                                      const std::function<bool(uint64_t command_buffer)>& rerecord)
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
        std::vector<std::pair<uint64_t, uint64_t>> modeled_waits{};
        for (uint32_t i = 0; i < wait_count; ++i)
        {
            const auto& wait = static_cast<const gpu_bridge::submit_semaphore_entry*>(wait_entries)[i];
            modeled_waits.emplace_back(wait.semaphore, wait.value);
        }
        if (!this->impl_->rerecord_changed(queue_it->second.handle, modeled_waits, command_ids, command_buffer_count, rerecord))
        {
            return VK_ERROR_DEVICE_LOST;
        }
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
            this->impl_->submit_modeled(queue_it->second.handle, queue, command_ids, command_buffer_count,
                                        static_cast<const gpu_bridge::submit_semaphore_entry*>(wait_entries), wait_count,
                                        static_cast<const gpu_bridge::submit_semaphore_entry*>(signal_entries), signal_count);
            this->impl_->track_submitted_semaphores(static_cast<const gpu_bridge::submit_semaphore_entry*>(wait_entries), wait_count,
                                                    static_cast<const gpu_bridge::submit_semaphore_entry*>(signal_entries), signal_count);
        }
        return result;
    }

    int32_t vulkan_host::queue_submit2(uint64_t queue, uint64_t fence, const void* wait_entries, uint32_t wait_count,
                                       const void* command_buffer_ids, uint32_t command_buffer_count, const void* signal_entries,
                                       uint32_t signal_count, const std::function<bool(uint64_t command_buffer)>& rerecord)
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
        std::vector<std::pair<uint64_t, uint64_t>> modeled_waits{};
        for (uint32_t i = 0; i < wait_count; ++i)
        {
            const auto& wait = static_cast<const gpu_bridge::submit2_semaphore_entry*>(wait_entries)[i];
            modeled_waits.emplace_back(wait.semaphore, wait.value);
        }
        if (!this->impl_->rerecord_changed(queue_it->second.handle, modeled_waits, cmd_ids, command_buffer_count, rerecord))
        {
            return VK_ERROR_DEVICE_LOST;
        }
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
            this->impl_->submit_modeled(queue_it->second.handle, queue, cmd_ids, command_buffer_count,
                                        static_cast<const gpu_bridge::submit2_semaphore_entry*>(wait_entries), wait_count,
                                        static_cast<const gpu_bridge::submit2_semaphore_entry*>(signal_entries), signal_count);
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

        const auto properties = this->impl_->get_guest_memory_types(instance->second, pd->second.handle).properties;
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
        const auto host_type_index = dev->second.memory_types.to_host(memory_type_index);
        if (have_props && host_type_index >= mem_props.memoryTypeCount)
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
        info.memoryTypeIndex = this->impl_->substitute_cached_memory_type(dev->second, host_type_index);

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
        out_memory_type_bits = dev->second.memory_types.to_guest_bits(requirements.memoryTypeBits);
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

        if (!dev->second.memory_types.is_host_visible(mem->second.memory_type_index))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
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
        return dev != this->impl_->devices.end() && dev->second.memory_types.is_host_visible(memory_type_index);
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

        const auto type_index =
            this->impl_->substitute_cached_memory_type(dev->second, dev->second.memory_types.to_host(memory_type_index));

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

    int32_t vulkan_host::flush_imported_memory(const std::span<const memory_range> ranges)
    {
        VkResult result = VK_SUCCESS;
        for (const auto& [device_id, mapped_ranges] : this->impl_->to_mapped_ranges(ranges))
        {
            const auto dev = this->impl_->devices.find(device_id);
            if (dev != this->impl_->devices.end() && dev->second.flush_mapped_memory_ranges)
            {
                const auto flushed = dev->second.flush_mapped_memory_ranges(dev->second.handle, static_cast<uint32_t>(mapped_ranges.size()),
                                                                            mapped_ranges.data());
                if (result == VK_SUCCESS)
                {
                    result = flushed;
                }
            }
        }

        return result;
    }

    int32_t vulkan_host::invalidate_imported_memory(const std::span<const memory_range> ranges)
    {
        VkResult result = VK_SUCCESS;
        for (const auto& [device_id, mapped_ranges] : this->impl_->to_mapped_ranges(ranges))
        {
            const auto dev = this->impl_->devices.find(device_id);
            if (dev != this->impl_->devices.end() && dev->second.invalidate_mapped_memory_ranges)
            {
                const auto invalidated = dev->second.invalidate_mapped_memory_ranges(
                    dev->second.handle, static_cast<uint32_t>(mapped_ranges.size()), mapped_ranges.data());
                if (result == VK_SUCCESS)
                {
                    result = invalidated;
                }
            }
        }

        return result;
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

        if (!dev->second.memory_types.is_host_visible(mem->second.memory_type_index))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
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

        // Multisampled images are read by sampling them and written by rendering to them.
        const VkImageUsageFlags transfer_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                 (info.samples > VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
        constexpr VkImageUsageFlags attachment_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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

        if (info.samples > VK_SAMPLE_COUNT_1_BIT && (info.usage & attachment_usage) == 0)
        {
            copyable = false;
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
        data.reset_subresources(static_cast<size_t>(data.aspect_count()) * info.mipLevels * info.arrayLayers);
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
        out_memory_type_bits = dev->second.memory_types.to_guest_bits(requirements.memoryTypeBits);
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

        // Hosts below Vulkan 1.2 may lack vkCreateRenderPass2. Version 1 render passes cannot express separate stencil
        // layouts, depth-stencil resolves or synchronization2 dependencies, so those are refused there.
        VkResult create_legacy_render_pass(const PFN_vkCreateRenderPass create, const VkDevice device,
                                           const vulkan_host::render_pass_description& description, VkRenderPass& render_pass)
        {
            const bool representable =
                std::ranges::none_of(description.attachments, [](const auto& a) { return a.has_stencil_layouts != 0; }) &&
                std::ranges::none_of(description.references, [](const auto& r) { return r.stencil_layout != VK_IMAGE_LAYOUT_UNDEFINED; }) &&
                std::ranges::none_of(description.subpasses, [](const auto& sp) { return sp.has_depth_stencil_resolve != 0; }) &&
                std::ranges::none_of(description.dependencies, [](const auto& d) { return d.has_memory_barrier2 != 0; });
            if (!representable)
            {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }

            std::vector<VkAttachmentDescription> attachments{};
            attachments.reserve(description.attachments.size());
            for (const auto& a : description.attachments)
            {
                attachments.push_back(VkAttachmentDescription{
                    .flags = a.flags,
                    .format = static_cast<VkFormat>(a.format),
                    .samples = static_cast<VkSampleCountFlagBits>(a.samples),
                    .loadOp = static_cast<VkAttachmentLoadOp>(a.load_op),
                    .storeOp = static_cast<VkAttachmentStoreOp>(a.store_op),
                    .stencilLoadOp = static_cast<VkAttachmentLoadOp>(a.stencil_load_op),
                    .stencilStoreOp = static_cast<VkAttachmentStoreOp>(a.stencil_store_op),
                    .initialLayout = translate_layout(a.initial_layout),
                    .finalLayout = translate_layout(a.final_layout),
                });
            }

            std::vector<VkAttachmentReference> references{};
            references.reserve(description.references.size());
            for (const auto& r : description.references)
            {
                references.push_back(VkAttachmentReference{.attachment = r.attachment, .layout = translate_layout(r.layout)});
            }

            std::vector<VkSubpassDescription> subpasses{};
            subpasses.reserve(description.subpasses.size());
            std::vector<uint32_t> view_masks{};
            view_masks.reserve(description.subpasses.size());
            std::vector<VkInputAttachmentAspectReference> input_aspects{};
            size_t next_reference = 0;
            size_t next_preserve = 0;
            const auto take = [](const auto& items, size_t& offset, const size_t count) {
                const auto* taken = count ? items.data() + offset : nullptr;
                offset += count;
                return taken;
            };
            for (const auto& sp : description.subpasses)
            {
                for (uint32_t i = 0; i < sp.input_count; ++i)
                {
                    const auto& reference = description.references[next_reference + i];
                    const auto aspect = reference.aspect_mask;
                    if (reference.attachment != VK_ATTACHMENT_UNUSED && aspect != 0 &&
                        aspect != get_vulkan_format_aspects(description.attachments[reference.attachment].format))
                    {
                        input_aspects.push_back(VkInputAttachmentAspectReference{
                            .subpass = static_cast<uint32_t>(subpasses.size()), .inputAttachmentIndex = i, .aspectMask = aspect});
                    }
                }

                VkSubpassDescription out{};
                out.flags = sp.flags;
                out.pipelineBindPoint = static_cast<VkPipelineBindPoint>(sp.bind_point);
                out.inputAttachmentCount = sp.input_count;
                out.pInputAttachments = take(references, next_reference, sp.input_count);
                out.colorAttachmentCount = sp.color_count;
                out.pColorAttachments = take(references, next_reference, sp.color_count);
                out.pResolveAttachments = sp.has_color_resolve ? take(references, next_reference, sp.color_count) : nullptr;
                out.pDepthStencilAttachment = sp.has_depth_stencil ? take(references, next_reference, 1) : nullptr;
                out.preserveAttachmentCount = sp.preserve_count;
                out.pPreserveAttachments = take(description.preserves, next_preserve, sp.preserve_count);
                subpasses.push_back(out);
                view_masks.push_back(sp.view_mask);
            }

            std::vector<VkSubpassDependency> dependencies{};
            dependencies.reserve(description.dependencies.size());
            std::vector<int32_t> view_offsets{};
            view_offsets.reserve(description.dependencies.size());
            for (const auto& d : description.dependencies)
            {
                dependencies.push_back(VkSubpassDependency{
                    .srcSubpass = d.src_subpass,
                    .dstSubpass = d.dst_subpass,
                    .srcStageMask = d.src_stage_mask,
                    .dstStageMask = d.dst_stage_mask,
                    .srcAccessMask = d.src_access_mask,
                    .dstAccessMask = d.dst_access_mask,
                    .dependencyFlags = d.flags,
                });
                view_offsets.push_back(d.view_offset);
            }

            VkRenderPassMultiviewCreateInfo multiview{};
            multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
            multiview.subpassCount = static_cast<uint32_t>(view_masks.size());
            multiview.pViewMasks = view_masks.data();
            multiview.dependencyCount = static_cast<uint32_t>(view_offsets.size());
            multiview.pViewOffsets = view_offsets.data();
            multiview.correlationMaskCount = static_cast<uint32_t>(description.correlated_view_masks.size());
            multiview.pCorrelationMasks = description.correlated_view_masks.data();
            const bool uses_multiview = std::ranges::any_of(view_masks, [](const uint32_t mask) { return mask != 0; });

            VkRenderPassInputAttachmentAspectCreateInfo aspects{};
            aspects.sType = VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO;
            aspects.pNext = uses_multiview ? &multiview : nullptr;
            aspects.aspectReferenceCount = static_cast<uint32_t>(input_aspects.size());
            aspects.pAspectReferences = input_aspects.data();

            VkRenderPassCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.pNext = aspects.pNext;
            if (!input_aspects.empty())
            {
                info.pNext = &aspects;
            }
            info.flags = description.flags;
            info.attachmentCount = static_cast<uint32_t>(attachments.size());
            info.pAttachments = attachments.data();
            info.subpassCount = static_cast<uint32_t>(subpasses.size());
            info.pSubpasses = subpasses.data();
            info.dependencyCount = static_cast<uint32_t>(dependencies.size());
            info.pDependencies = dependencies.data();

            return create(device, &info, nullptr, &render_pass);
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
        barrier.oldLayout = img->second.host_layout(old_layout);
        barrier.newLayout = img->second.host_layout(new_layout);
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
            .source_family = src_queue_family_index,
            .destination_family = dst_queue_family_index,
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
            pd->second.instance_id != surf->second.instance_id || !surf->second.handle)
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
            pd->second.instance_id != surf->second.instance_id || !surf->second.handle)
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
        // Windows always offers opaque composition. A swapchain the host surface lacks it for is presented through proxy
        // images.
        caps.supportedCompositeAlpha |= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
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
            pd->second.instance_id != surf->second.instance_id || !surf->second.handle)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
        }

        const auto instance = this->impl_->instances.find(pd->second.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_formats)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        std::vector<VkSurfaceFormatKHR> formats{};
        if (const VkResult result = impl::query_surface_formats(instance->second, pd->second.handle, surf->second.handle, formats);
            result != VK_SUCCESS)
        {
            return result;
        }

        // Windows always offers B8G8R8A8, which guests such as Zink's WGL backend use without asking. A swapchain the
        // host surface lacks it for is presented through proxy images.
        const auto native_count = formats.size();
        for (size_t i = 0; i < native_count; ++i)
        {
            const VkSurfaceFormatKHR swapped{.format = impl::channel_swapped_format(formats[i].format),
                                             .colorSpace = formats[i].colorSpace};
            if (swapped.format != VK_FORMAT_UNDEFINED && std::ranges::none_of(formats, [&](const VkSurfaceFormatKHR& offered) {
                    return offered.format == swapped.format && offered.colorSpace == swapped.colorSpace;
                }))
            {
                formats.push_back(swapped);
            }
        }

        out_count = static_cast<uint32_t>(formats.size());
        for (size_t i = 0; i < formats.size() && i < out_formats.size(); ++i)
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
            pd->second.instance_id != surf->second.instance_id || !surf->second.handle)
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
        if (!surf_it->second.handle)
        {
            return VK_ERROR_SURFACE_LOST_KHR;
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

        const auto instance = this->impl_->instances.find(dev.instance_id);
        if (instance == this->impl_->instances.end() || !instance->second.get_surface_capabilities ||
            !instance->second.get_surface_formats || !instance->second.get_surface_present_modes)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkSurfaceCapabilitiesKHR caps{};
        std::vector<VkSurfaceFormatKHR> native_formats{};
        std::vector<VkPresentModeKHR> native_modes{};
        VkResult queried = instance->second.get_surface_capabilities(dev.physical_device, surf_it->second.handle, &caps);
        if (queried == VK_SUCCESS)
        {
            queried = impl::query_surface_formats(instance->second, dev.physical_device, surf_it->second.handle, native_formats);
        }
        if (queried == VK_SUCCESS)
        {
            queried = impl::query_surface_present_modes(instance->second, dev.physical_device, surf_it->second.handle, native_modes);
        }
        if (queried != VK_SUCCESS)
        {
            return queried;
        }

        const auto requested_format = static_cast<VkFormat>(format);
        const auto offered = [&](const VkFormat candidate) {
            return std::ranges::any_of(native_formats, [&](const VkSurfaceFormatKHR& native) {
                return native.format == candidate && native.colorSpace == static_cast<VkColorSpaceKHR>(color_space);
            });
        };
        const bool format_native = offered(requested_format);
        const bool alpha_native = (caps.supportedCompositeAlpha & composite_alpha) != 0;
        const bool count_native =
            min_image_count >= caps.minImageCount && (caps.maxImageCount == 0 || min_image_count <= caps.maxImageCount);
        const bool mode_native = std::ranges::find(native_modes, static_cast<VkPresentModeKHR>(present_mode)) != native_modes.end();
        const bool proxied = !format_native || !alpha_native || !count_native || !mode_native;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.flags = static_cast<VkSwapchainCreateFlagsKHR>(flags);
        info.surface = surf_it->second.handle;
        info.minImageCount = min_image_count;
        info.imageFormat = requested_format;
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

        if (proxied)
        {
            const VkFormat native_format = format_native ? requested_format : impl::channel_swapped_format(requested_format);
            if (!offered(native_format))
            {
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            if (const VkResult supported =
                    impl::check_swapchain_proxy_support(dev, instance->second, requested_format, native_format, flags, caps);
                supported != VK_SUCCESS)
            {
                return supported;
            }

            const auto supported_alpha = static_cast<VkCompositeAlphaFlagsKHR>(caps.supportedCompositeAlpha);
            info.flags = 0;
            info.minImageCount = caps.minImageCount;
            info.imageFormat = native_format;
            info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.queueFamilyIndexCount = 0;
            info.pQueueFamilyIndices = nullptr;
            info.compositeAlpha = alpha_native ? static_cast<VkCompositeAlphaFlagBitsKHR>(composite_alpha)
                                               : static_cast<VkCompositeAlphaFlagBitsKHR>(supported_alpha & (~supported_alpha + 1));
            info.presentMode = mode_native ? static_cast<VkPresentModeKHR>(present_mode) : VK_PRESENT_MODE_FIFO_KHR;
        }

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

        if (proxied)
        {
            sc.proxy.emplace();
            result = this->impl_->create_swapchain_proxy(dev, impl::proxy_image_info(sc.create),
                                                         std::max({min_image_count, caps.minImageCount, 1U}), native_images, *sc.proxy);
            if (result != VK_SUCCESS)
            {
                impl::destroy_swapchain_proxy(dev, *sc.proxy);
                dev.destroy_swapchain(dev.handle, native_swapchain, nullptr);
                return result;
            }
        }

        this->impl_->register_swapchain_images(sc, sc.proxy ? sc.proxy->images : native_images);

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

    int32_t vulkan_host::acquire_next_image(uint64_t swapchain, uint64_t timeout, uint64_t semaphore, uint64_t fence, uint32_t& out_index,
                                            const std::function<void(const std::function<void()>&)>& run_unlocked)
    {
        out_index = 0;
        auto sc_it = this->impl_->swapchains.find(swapchain);
        if (sc_it == this->impl_->swapchains.end())
        {
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        // Waiting for a copy out of a proxy image must not hold up the other vCPUs. The objects are looked up again
        // afterwards, since the guest may destroy them meanwhile.
        std::optional<uint32_t> proxy_index{};
        if (sc_it->second.proxy)
        {
            const auto start = std::chrono::steady_clock::now();
            while (true)
            {
                sc_it = this->impl_->swapchains.find(swapchain);
                if (sc_it == this->impl_->swapchains.end() || sc_it->second.proxy->lost)
                {
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }
                const auto device = this->impl_->devices.find(sc_it->second.device_id);
                if (device == this->impl_->devices.end())
                {
                    return VK_ERROR_DEVICE_LOST;
                }

                bool copying{};
                VkResult status{};
                proxy_index = impl::find_free_proxy_image(device->second, sc_it->second, copying, status);
                if (proxy_index)
                {
                    break;
                }
                if (status != VK_SUCCESS)
                {
                    return status;
                }
                // With every proxy image acquired, one may still be on its way through a present running unlocked.
                if (timeout == 0)
                {
                    return VK_NOT_READY;
                }
                if (static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) >= timeout)
                {
                    return VK_TIMEOUT;
                }
                run_unlocked([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
            }
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

        VkResult result{};
        if (proxy_index)
        {
            result = this->impl_->signal_proxy_acquire(dev_it->second, sc_it->second.device_id, semaphore_handle, fence_handle);
            out_index = *proxy_index;
        }
        else
        {
            result = dev_it->second.acquire_next_image(dev_it->second.handle, sc_it->second.handle, timeout, semaphore_handle, fence_handle,
                                                       &out_index);
        }

        if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && out_index < sc_it->second.acquired.size())
        {
            auto& image = this->impl_->images.at(sc_it->second.image_ids[out_index]);
            if (image.layout_undefined &&
                !this->impl_->transition_restored_swapchain_image(dev_it->second, image, semaphore_handle, fence_handle))
            {
                return VK_ERROR_DEVICE_LOST;
            }

            sc_it->second.acquired[out_index] = true;
            if (semaphore != 0)
            {
                auto& acquired = this->impl_->semaphores.at(semaphore);
                acquired.binary_signaled = true;
                ++acquired.modeled_signals;
                this->impl_->run_modeled();
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

        if (auto& proxy = sc_it->second.proxy)
        {
            if (proxy->lost)
            {
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
            if (const VkResult prepared = this->impl_->prepare_proxy_copies(dev_it->second, *proxy, queue_it->second.queue_family_index);
                prepared != VK_SUCCESS)
            {
                return prepared;
            }
        }

        VkSwapchainKHR native_swapchain = sc_it->second.handle;
        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = static_cast<uint32_t>(wait_handles.size());
        info.pWaitSemaphores = wait_handles.empty() ? nullptr : wait_handles.data();
        info.swapchainCount = 1;
        info.pSwapchains = &native_swapchain;
        info.pImageIndices = &image_index;

        const uint64_t present_device = queue_it->second.device_id;
        VkFence present_fence{};
        VkSwapchainPresentFenceInfoEXT present_fence_info{};
        if (dev_it->second.present_fences)
        {
            if (const VkResult fence_result = impl::take_present_fence(dev_it->second, sc_it->second, present_fence);
                fence_result != VK_SUCCESS)
            {
                return fence_result;
            }

            present_fence_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            present_fence_info.swapchainCount = 1;
            present_fence_info.pFences = &present_fence;
            info.pNext = &present_fence_info;
        }

        const auto native_present = dev_it->second.queue_present;
        auto* const native_queue = queue_it->second.handle;
        const auto queue_lock = this->impl_->queue_lock(native_queue);
        const auto presents = dev_it->second.presents;
        std::shared_lock device_in_use(*presents);

        // A proxied present copies the guest's image into a native one, which it has to acquire first. That acquire
        // waits for the presentation engine like a native present does, so it runs unlocked as well.
        struct proxy_copy
        {
            VkImage source{};
            VkCommandBuffer command_buffer{};
            VkFence copied{};
            VkSemaphore native_acquired{};
            std::shared_ptr<std::mutex> native_access{};
            std::vector<VkSemaphore> native_ready{};
            std::vector<VkImage> native_images{};
            impl::image_create_params extent{};
        };

        std::optional<proxy_copy> copy{};
        if (const auto& proxy = sc_it->second.proxy)
        {
            const uint32_t family = queue_it->second.queue_family_index;
            copy = proxy_copy{.source = proxy->images[image_index],
                              .command_buffer = proxy->copies.at(family).buffers[image_index],
                              .copied = proxy->copied[image_index],
                              .native_acquired = proxy->native_acquired[image_index],
                              .native_access = proxy->native_access,
                              .native_ready = proxy->native_ready,
                              .native_images = proxy->native_images,
                              .extent = this->impl_->images.at(sc_it->second.image_ids[image_index]).create};
        }
        const auto& copy_device = dev_it->second;
        constexpr VkPipelineStageFlags copy_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const std::vector<VkPipelineStageFlags> copy_wait_stages(wait_handles.size() + 1, copy_stage);

        // The device lock is released before run_unlocked returns, since its caller may then wait for a lock that a
        // thread destroying the device holds.
        bool copy_submitted{};
        bool presented_natively{};
        bool native_image_stranded{};
        const int32_t result = run_unlocked([&] {
            int32_t present_result{};
            uint32_t native_index = image_index;
            VkSemaphore ready = VK_NULL_HANDLE;
            if (copy)
            {
                const std::scoped_lock swapchain_guard(*copy->native_access);
                const VkResult acquired = copy_device.acquire_next_image(copy_device.handle, native_swapchain, UINT64_MAX,
                                                                         copy->native_acquired, VK_NULL_HANDLE, &native_index);
                const bool copying = acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR;

                VkCommandBufferBeginInfo begin_info{};
                begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                auto* const command_buffer = copy->command_buffer;
                if (copying)
                {
                    ready = copy->native_ready[native_index];
                    if (const VkResult recorded = copy_device.begin_command_buffer(command_buffer, &begin_info); recorded != VK_SUCCESS)
                    {
                        native_image_stranded = true;
                        device_in_use.unlock();
                        return static_cast<int32_t>(recorded);
                    }
                    impl::record_proxy_copy(copy_device, command_buffer, copy->source, copy->native_images[native_index], copy->extent);
                    if (const VkResult recorded = copy_device.end_command_buffer(command_buffer); recorded != VK_SUCCESS)
                    {
                        native_image_stranded = true;
                        device_in_use.unlock();
                        return static_cast<int32_t>(recorded);
                    }
                }

                // Without a native image the guest's semaphores are still waited for, as a failed present consumes them.
                std::vector<VkSemaphore> copy_waits = wait_handles;
                if (copying)
                {
                    copy_waits.push_back(copy->native_acquired);
                }
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.waitSemaphoreCount = static_cast<uint32_t>(copy_waits.size());
                submit.pWaitSemaphores = copy_waits.empty() ? nullptr : copy_waits.data();
                submit.pWaitDstStageMask = copy_wait_stages.data();
                submit.commandBufferCount = copying ? 1 : 0;
                submit.pCommandBuffers = &command_buffer;
                submit.signalSemaphoreCount = copying ? 1 : 0;
                submit.pSignalSemaphores = &ready;

                const std::scoped_lock guard(*queue_lock);
                VkResult submitted = copy_device.reset_fences(copy_device.handle, 1, &copy->copied);
                if (submitted == VK_SUCCESS)
                {
                    submitted = copy_device.queue_submit(native_queue, 1, &submit, copy->copied);
                }
                copy_submitted = submitted == VK_SUCCESS;
                native_image_stranded = copying && !copy_submitted;
                if (submitted != VK_SUCCESS || !copying)
                {
                    device_in_use.unlock();
                    return static_cast<int32_t>(submitted != VK_SUCCESS ? submitted : acquired);
                }

                info.waitSemaphoreCount = 1;
                info.pWaitSemaphores = &ready;
                info.pImageIndices = &native_index;
                present_result = native_present(native_queue, &info);
                presented_natively = true;
                if (present_result == VK_SUCCESS && acquired == VK_SUBOPTIMAL_KHR)
                {
                    present_result = VK_SUBOPTIMAL_KHR;
                }
            }
            else
            {
                const std::scoped_lock guard(*queue_lock);
                present_result = native_present(native_queue, &info);
                presented_natively = true;
            }
            device_in_use.unlock();
            return present_result;
        });

        // Only these leave the present unqueued, with the image still acquired and the semaphores untouched.
        const bool queued = result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (present_fence)
        {
            if (const auto presented = this->impl_->swapchains.find(swapchain); presented != this->impl_->swapchains.end())
            {
                (queued && presented_natively ? presented->second.presenting : presented->second.spare_present_fences)
                    .push_back(present_fence);
            }
            else if (const auto device_it = this->impl_->devices.find(present_device); device_it != this->impl_->devices.end())
            {
                const auto& dev = device_it->second;
                const VkResult waited =
                    presented_natively ? dev.wait_for_fences(dev.handle, 1, &present_fence, VK_TRUE, UINT64_MAX) : VK_SUCCESS;
                if (waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST)
                {
                    dev.destroy_fence(dev.handle, present_fence, nullptr);
                }
            }
        }

        if (const auto presented = this->impl_->swapchains.find(swapchain);
            presented != this->impl_->swapchains.end() && presented->second.proxy)
        {
            presented->second.proxy->copy_submitted[image_index] = copy_submitted;
            presented->second.proxy->lost |= native_image_stranded;
        }

        if (queued)
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

            impl::pending_submission present{.queue_id = queue};
            for (const uint64_t semaphore : wait_semaphores)
            {
                present.waits.emplace_back(semaphore, 0);
            }
            this->impl_->pending_presents[native_queue].push_back(std::move(present));
            this->impl_->quiesced = false;
            this->impl_->run_modeled();
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
        uint64_t value_count = qp->second.value_count();
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

                const VkResult single = dev->second.get_query_pool_results(dev->second.handle, qp->second.handle, first_query + i, 1,
                                                                           static_cast<size_t>(element_size), element, element_size,
                                                                           static_cast<VkQueryResultFlags>(flags));
                if (single != VK_SUCCESS && result == VK_SUCCESS)
                {
                    result = single;
                }
            }

            if (result == VK_SUCCESS || result == VK_NOT_READY)
            {
                out_written = out_size;
            }
            return result;
        }

        const VkResult result = dev->second.get_query_pool_results(dev->second.handle, qp->second.handle, first_query, query_count,
                                                                   out_size, out, stride, static_cast<VkQueryResultFlags>(flags));
        if (result == VK_SUCCESS || result == VK_NOT_READY)
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
        cb->second.query_reset_positions.push_back(cb->second.layout_transitions.size());
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
            const auto query = first_query + i;
            const auto offset = destination_offset + (i * stride);
            const bool reset_before = std::ranges::any_of(cb->second.query_resets, [&](const auto& reset) {
                const auto& [pool, first, count] = reset;
                return pool == query_pool && query >= first && query - first < count;
            });

            if (reset_before || this->impl_->reset_ahead(query_pool, query) || !qp->second.restored_results.contains(query))
            {
                dev->second.cmd_copy_query_pool_results(cb->second.handle, qp->second.handle, first_query + i, 1, buffer->second.handle,
                                                        offset, stride, static_cast<VkQueryResultFlags>(flags));
                continue;
            }

            const auto restored = qp->second.restored_result(query, flags);
            dev->second.cmd_update_buffer(cb->second.handle, buffer->second.handle, offset, restored.size(), restored.data());
            cb->second.restored_query_reads.emplace_back(query_pool, query);
        }
        return VK_SUCCESS;
    }

    namespace
    {
        // Expects a description create_render_pass validated.
        VkResult create_native_render_pass(const PFN_vkCreateRenderPass2 create2, const PFN_vkCreateRenderPass create,
                                           const VkDevice device, const vulkan_host::render_pass_description& description,
                                           VkRenderPass& out_render_pass)
        {
            if (!create2)
            {
                return create_legacy_render_pass(create, device, description, out_render_pass);
            }

            const auto attachment_count = static_cast<uint32_t>(description.attachments.size());
            const auto subpass_count = static_cast<uint32_t>(description.subpasses.size());

            std::vector<VkAttachmentDescription2> attachments(attachment_count);
            std::vector<VkAttachmentDescriptionStencilLayout> stencil_layouts(attachment_count);
            for (uint32_t i = 0; i < attachment_count; ++i)
            {
                const auto& a = description.attachments[i];
                auto& out = attachments[i];
                out.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                out.flags = a.flags;
                out.format = static_cast<VkFormat>(a.format);
                out.samples = static_cast<VkSampleCountFlagBits>(a.samples);
                out.loadOp = static_cast<VkAttachmentLoadOp>(a.load_op);
                out.storeOp = static_cast<VkAttachmentStoreOp>(a.store_op);
                out.stencilLoadOp = static_cast<VkAttachmentLoadOp>(a.stencil_load_op);
                out.stencilStoreOp = static_cast<VkAttachmentStoreOp>(a.stencil_store_op);
                out.initialLayout = translate_layout(a.initial_layout);
                out.finalLayout = translate_layout(a.final_layout);
                if (a.has_stencil_layouts)
                {
                    stencil_layouts[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT;
                    stencil_layouts[i].stencilInitialLayout = translate_layout(a.stencil_initial_layout);
                    stencil_layouts[i].stencilFinalLayout = translate_layout(a.stencil_final_layout);
                    out.pNext = &stencil_layouts[i];
                }
            }

            std::vector<VkAttachmentReference2> references(description.references.size());
            std::vector<VkAttachmentReferenceStencilLayout> reference_stencil_layouts(description.references.size());
            for (size_t i = 0; i < references.size(); ++i)
            {
                const auto& r = description.references[i];
                references[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
                references[i].attachment = r.attachment;
                references[i].layout = translate_layout(r.layout);
                references[i].aspectMask = r.aspect_mask;
                if (r.stencil_layout != VK_IMAGE_LAYOUT_UNDEFINED)
                {
                    reference_stencil_layouts[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT;
                    reference_stencil_layouts[i].stencilLayout = translate_layout(r.stencil_layout);
                    references[i].pNext = &reference_stencil_layouts[i];
                }
            }

            std::vector<VkSubpassDescription2> subpasses(subpass_count);
            std::vector<VkSubpassDescriptionDepthStencilResolve> resolves(subpass_count);
            size_t next_reference = 0;
            size_t next_preserve = 0;
            const auto take = [](const auto& items, size_t& offset, const size_t count) {
                const auto* taken = count ? items.data() + offset : nullptr;
                offset += count;
                return taken;
            };
            for (uint32_t i = 0; i < subpass_count; ++i)
            {
                const auto& sp = description.subpasses[i];
                auto& out = subpasses[i];
                out.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
                out.flags = sp.flags;
                out.pipelineBindPoint = static_cast<VkPipelineBindPoint>(sp.bind_point);
                out.viewMask = sp.view_mask;
                out.inputAttachmentCount = sp.input_count;
                out.pInputAttachments = take(references, next_reference, sp.input_count);
                out.colorAttachmentCount = sp.color_count;
                out.pColorAttachments = take(references, next_reference, sp.color_count);
                out.pResolveAttachments = sp.has_color_resolve ? take(references, next_reference, sp.color_count) : nullptr;
                out.pDepthStencilAttachment = sp.has_depth_stencil ? take(references, next_reference, 1) : nullptr;
                if (sp.has_depth_stencil_resolve)
                {
                    resolves[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
                    resolves[i].depthResolveMode = static_cast<VkResolveModeFlagBits>(sp.depth_resolve_mode);
                    resolves[i].stencilResolveMode = static_cast<VkResolveModeFlagBits>(sp.stencil_resolve_mode);
                    resolves[i].pDepthStencilResolveAttachment = take(references, next_reference, 1);
                    out.pNext = &resolves[i];
                }
                out.preserveAttachmentCount = sp.preserve_count;
                out.pPreserveAttachments = take(description.preserves, next_preserve, sp.preserve_count);
            }

            std::vector<VkSubpassDependency2> dependencies(description.dependencies.size());
            std::vector<VkMemoryBarrier2> barriers(description.dependencies.size());
            for (size_t i = 0; i < dependencies.size(); ++i)
            {
                const auto& d = description.dependencies[i];
                auto& out = dependencies[i];
                out.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
                if (d.has_memory_barrier2)
                {
                    barriers[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                    barriers[i].srcStageMask = d.src_stage_mask2;
                    barriers[i].dstStageMask = d.dst_stage_mask2;
                    barriers[i].srcAccessMask = d.src_access_mask2;
                    barriers[i].dstAccessMask = d.dst_access_mask2;
                    out.pNext = &barriers[i];
                }
                out.srcSubpass = d.src_subpass;
                out.dstSubpass = d.dst_subpass;
                out.srcStageMask = d.src_stage_mask;
                out.dstStageMask = d.dst_stage_mask;
                out.srcAccessMask = d.src_access_mask;
                out.dstAccessMask = d.dst_access_mask;
                out.dependencyFlags = d.flags;
                out.viewOffset = d.view_offset;
            }

            VkRenderPassCreateInfo2 info{};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
            info.flags = description.flags;
            info.attachmentCount = attachment_count;
            info.pAttachments = attachments.data();
            info.subpassCount = subpass_count;
            info.pSubpasses = subpasses.data();
            info.dependencyCount = static_cast<uint32_t>(dependencies.size());
            info.pDependencies = dependencies.data();
            info.correlatedViewMaskCount = static_cast<uint32_t>(description.correlated_view_masks.size());
            info.pCorrelatedViewMasks = description.correlated_view_masks.data();

            return create2(device, &info, nullptr, &out_render_pass);
        }
    }

    int32_t vulkan_host::create_render_pass(uint64_t device, const render_pass_description& description, uint64_t& out_render_pass)
    {
        out_render_pass = 0;
        const auto dev = this->impl_->devices.find(device);
        if (dev == this->impl_->devices.end() || (!dev->second.create_render_pass2 && !dev->second.create_render_pass))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto attachment_count = static_cast<uint32_t>(description.attachments.size());
        const auto subpass_count = static_cast<uint32_t>(description.subpasses.size());
        const auto valid_attachment = [&](const uint32_t attachment) {
            return attachment == VK_ATTACHMENT_UNUSED || attachment < attachment_count;
        };
        const auto valid_subpass = [&](const uint32_t subpass) { return subpass == VK_SUBPASS_EXTERNAL || subpass < subpass_count; };

        size_t reference_total = 0;
        size_t preserve_total = 0;
        for (const auto& subpass : description.subpasses)
        {
            reference_total += static_cast<size_t>(subpass.input_count) + subpass.color_count +
                               (subpass.has_color_resolve ? subpass.color_count : 0) + (subpass.has_depth_stencil ? 1 : 0) +
                               (subpass.has_depth_stencil_resolve ? 1 : 0);
            preserve_total += subpass.preserve_count;
        }
        if (subpass_count == 0 || reference_total != description.references.size() || preserve_total != description.preserves.size() ||
            !std::ranges::all_of(description.references, [&](const render_pass_reference& r) { return valid_attachment(r.attachment); }) ||
            !std::ranges::all_of(description.preserves, [&](const uint32_t a) { return a < attachment_count; }) ||
            !std::ranges::all_of(description.dependencies, [&](const render_pass_dependency& d) {
                return valid_subpass(d.src_subpass) && valid_subpass(d.dst_subpass);
            }))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const auto register_render_pass = [&](const VkRenderPass render_pass) {
            const uint64_t id = this->impl_->allocate_id();
            this->impl_->render_passes.emplace(
                id, impl::render_pass_data{.handle = render_pass, .device_id = device, .create = {.description = description}});
            out_render_pass = id;
            return VK_SUCCESS;
        };

        VkRenderPass render_pass{};
        const VkResult result = create_native_render_pass(dev->second.create_render_pass2, dev->second.create_render_pass,
                                                          dev->second.handle, description, render_pass);
        return result == VK_SUCCESS ? register_render_pass(render_pass) : result;
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
            for (const auto& [proxies, variant] : it->second.proxy_variants)
            {
                dev->second.destroy_render_pass(dev->second.handle, variant, nullptr);
            }
        }
        this->impl_->render_passes.erase(it);
    }

    int32_t vulkan_host::create_framebuffer(uint64_t device, uint64_t render_pass, uint32_t flags, uint32_t width, uint32_t height,
                                            uint32_t layers, std::span<const uint64_t> views,
                                            std::span<const framebuffer_attachment_image> images, uint64_t& out_framebuffer)
    {
        out_framebuffer = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto rp = this->impl_->render_passes.find(render_pass);
        if (dev == this->impl_->devices.end() || rp == this->impl_->render_passes.end() || rp->second.device_id != device ||
            !dev->second.create_framebuffer)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        const bool imageless = (flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
        const size_t attachment_count = imageless ? images.size() : views.size();
        if (attachment_count != rp->second.create.description.attachments.size() || (imageless && !views.empty()))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkImageView> view_handles{};
        view_handles.reserve(views.size());
        for (const uint64_t view_id : views)
        {
            const auto view = this->impl_->image_views.find(view_id);
            if (view == this->impl_->image_views.end() || view->second.device_id != device)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            view_handles.push_back(view->second.handle);
        }

        std::vector<std::vector<VkFormat>> view_formats(images.size());
        std::vector<VkFramebufferAttachmentImageInfo> image_infos(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            const auto& image = images[i];
            for (const uint32_t format : image.view_formats)
            {
                view_formats[i].push_back(static_cast<VkFormat>(format));
            }
            auto& info = image_infos[i];
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO;
            info.flags = image.flags;
            info.usage = image.usage;
            info.width = image.width;
            info.height = image.height;
            info.layerCount = image.layer_count;
            info.viewFormatCount = static_cast<uint32_t>(view_formats[i].size());
            info.pViewFormats = view_formats[i].data();
        }

        VkFramebufferAttachmentsCreateInfo attachments_info{};
        attachments_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO;
        attachments_info.attachmentImageInfoCount = static_cast<uint32_t>(image_infos.size());
        attachments_info.pAttachmentImageInfos = image_infos.data();

        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.pNext = imageless ? &attachments_info : nullptr;
        info.flags = flags;
        info.renderPass = rp->second.handle;
        info.attachmentCount = static_cast<uint32_t>(attachment_count);
        info.pAttachments = imageless ? nullptr : view_handles.data();
        info.width = width;
        info.height = height;
        info.layers = layers;

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
                                                                                .flags = flags,
                                                                                .width = width,
                                                                                .height = height,
                                                                                .layers = layers,
                                                                                .views = {views.begin(), views.end()},
                                                                                .images = {images.begin(), images.end()}}});
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
        info.pNext =
            std::ranges::any_of(vk_binding_flags, [](const VkDescriptorBindingFlags f) { return f != 0; }) ? &binding_flags : nullptr;
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
        info.pNext =
            std::ranges::any_of(vk_binding_flags, [](const VkDescriptorBindingFlags f) { return f != 0; }) ? &binding_flags : nullptr;
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
        static thread_local std::vector<impl::descriptor_write_storage> write_storage;
        static thread_local std::vector<VkCopyDescriptorSet> vk_copies;
        vk_writes.clear();
        write_storage.resize(writes.size());
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
            if (!this->impl_->build_descriptor_write(device, w, write_storage[i], vw))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            vw.dstSet = cached_set_handle;

            if (vw.pBufferInfo && w.buffer_or_view != 0)
            {
                cached_set->buffer_bindings[w.dst_binding] =
                    impl::bound_buffer_info{.buffer_id = w.buffer_or_view, .offset = w.offset, .range = w.range, .type = w.descriptor_type};
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

    int32_t vulkan_host::create_graphics_pipeline(uint64_t device, uint64_t pipeline_cache, uint64_t render_pass, uint32_t subpass,
                                                  uint64_t pipeline_layout, const shader_stage_source& vertex_shader,
                                                  const shader_stage_source& fragment_shader, uint32_t flags, uint32_t width,
                                                  uint32_t height, std::span<const vertex_binding> bindings,
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
        // Depth-only passes (depth prepass, shadows) have no fragment stage.
        const bool has_fragment_stage = fragment_shader.module != 0 || !fragment_shader.identifier.empty();
        if (!resolve_stage(vertex_shader, vertex_module) || (has_fragment_stage && !resolve_stage(fragment_shader, fragment_module)))
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
        auto color_count = static_cast<uint32_t>(color_formats.size());
        if (!dynamic_rendering)
        {
            const auto rp = this->impl_->render_passes.find(render_pass);
            if (rp == this->impl_->render_passes.end() || rp->second.device_id != device ||
                subpass >= rp->second.create.description.subpasses.size())
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            rp_handle = rp->second.handle;
            color_count = rp->second.create.description.subpasses[subpass].color_count;
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
        info.stageCount = has_fragment_stage ? 2 : 1;
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
        info.subpass = subpass;

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
        create.subpass = subpass;
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

    int32_t vulkan_host::cmd_begin_render_pass(uint64_t command_buffer, uint64_t render_pass, uint64_t framebuffer, const render_area& area,
                                               uint32_t contents, std::span<const std::array<uint32_t, 4>> clear_values,
                                               std::span<const uint64_t> attachments)
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

        const bool imageless = (fb->second.create.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
        const auto views = imageless ? attachments : std::span<const uint64_t>{fb->second.create.views};
        const auto& description = rp->second.create.description;
        if (views.size() != description.attachments.size() || (!imageless && !attachments.empty()))
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkImageView> view_handles{};
        view_handles.reserve(views.size());
        std::vector<bool> proxies(views.size(), false);
        for (size_t i = 0; i < views.size(); ++i)
        {
            const auto view = this->impl_->image_views.find(views[i]);
            if (view == this->impl_->image_views.end() || view->second.device_id != cb->second.device_id)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            view_handles.push_back(view->second.handle);

            const auto image = this->impl_->images.find(view->second.create.image);
            const auto& attachment = description.attachments[i];
            proxies[i] = image != this->impl_->images.end() && image->second.swapchain_proxy &&
                         (attachment.initial_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ||
                          attachment.final_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }

        VkRenderPass native_render_pass = rp->second.handle;
        if (std::ranges::any_of(proxies, [](const bool proxy) { return proxy; }))
        {
            auto& variant = rp->second.proxy_variants[proxies];
            if (!variant)
            {
                auto proxied = description;
                for (size_t i = 0; i < proxies.size(); ++i)
                {
                    auto& attachment = proxied.attachments[i];
                    for (auto* layout : {&attachment.initial_layout, &attachment.final_layout})
                    {
                        if (proxies[i] && *layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                        {
                            *layout = VK_IMAGE_LAYOUT_GENERAL;
                        }
                    }
                }
                if (const VkResult created = create_native_render_pass(dev->second.create_render_pass2, dev->second.create_render_pass,
                                                                       dev->second.handle, proxied, variant);
                    created != VK_SUCCESS)
                {
                    rp->second.proxy_variants.erase(proxies);
                    return created;
                }
            }
            native_render_pass = variant;
        }

        std::vector<VkClearValue> clears(clear_values.size());
        for (size_t i = 0; i < clears.size(); ++i)
        {
            static_assert(sizeof(VkClearValue) == sizeof(clear_values[i]));
            std::memcpy(&clears[i], clear_values[i].data(), sizeof(VkClearValue));
        }

        VkRenderPassAttachmentBeginInfo attachment_info{};
        attachment_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
        attachment_info.attachmentCount = static_cast<uint32_t>(view_handles.size());
        attachment_info.pAttachments = view_handles.data();

        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.pNext = imageless ? &attachment_info : nullptr;
        info.renderPass = native_render_pass;
        info.framebuffer = fb->second.handle;
        info.renderArea.offset = {.x = area.x, .y = area.y};
        info.renderArea.extent = {.width = area.width, .height = area.height};
        info.clearValueCount = static_cast<uint32_t>(clears.size());
        info.pClearValues = clears.data();

        dev->second.cmd_begin_render_pass(cb->second.handle, &info, static_cast<VkSubpassContents>(contents));

        for (size_t i = 0; i < views.size(); ++i)
        {
            const auto view = this->impl_->image_views.find(views[i]);
            const auto& create = view->second.create;
            const auto& attachment = description.attachments[i];
            const auto record = [&](const uint32_t aspect_mask, const uint32_t layout) {
                if (aspect_mask == 0)
                {
                    return;
                }
                cb->second.layout_transitions.push_back(impl::layout_transition{
                    .image = create.image,
                    .aspect_mask = aspect_mask,
                    .base_mip_level = create.base_mip_level,
                    .level_count = create.level_count ? create.level_count : 1,
                    .base_array_layer = create.base_array_layer,
                    .layer_count = create.layer_count ? create.layer_count : 1,
                    .new_layout = static_cast<uint32_t>(translate_layout(layout)),
                });
            };

            if (attachment.has_stencil_layouts)
            {
                record(create.aspect_mask & ~static_cast<uint32_t>(VK_IMAGE_ASPECT_STENCIL_BIT), attachment.final_layout);
                record(create.aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT, attachment.stencil_final_layout);
            }
            else
            {
                record(create.aspect_mask, attachment.final_layout);
            }
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

    int32_t vulkan_host::cmd_dispatch_base(uint64_t command_buffer, const std::array<uint32_t, 3>& base_group,
                                           const std::array<uint32_t, 3>& group_count)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_dispatch_base)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_dispatch_base(cb->second.handle, base_group[0], base_group[1], base_group[2], group_count[0], group_count[1],
                                      group_count[2]);
        return VK_SUCCESS;
    }

    int32_t vulkan_host::get_render_area_granularity(uint64_t device, uint64_t render_pass, uint32_t& out_width, uint32_t& out_height)
    {
        out_width = 0;
        out_height = 0;
        const auto dev = this->impl_->devices.find(device);
        const auto rp = this->impl_->render_passes.find(render_pass);
        if (dev == this->impl_->devices.end() || rp == this->impl_->render_passes.end() || rp->second.device_id != device ||
            !dev->second.get_render_area_granularity)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkExtent2D granularity{};
        dev->second.get_render_area_granularity(dev->second.handle, rp->second.handle, &granularity);
        out_width = granularity.width;
        out_height = granularity.height;
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

    int32_t vulkan_host::cmd_push_descriptor_set(const uint64_t command_buffer, const uint64_t pipeline_layout, const uint32_t bind_point,
                                                 const uint32_t set, const std::span<const descriptor_write> writes)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        const auto layout = this->impl_->pipeline_layouts.find(pipeline_layout);
        if (cb == this->impl_->command_buffers.end() || layout == this->impl_->pipeline_layouts.end() ||
            layout->second.device_id != cb->second.device_id)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_push_descriptor_set)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        static thread_local std::vector<VkWriteDescriptorSet> vk_writes;
        static thread_local std::vector<impl::descriptor_write_storage> write_storage;
        vk_writes.resize(writes.size());
        write_storage.resize(writes.size());
        for (size_t i = 0; i < writes.size(); ++i)
        {
            if (!this->impl_->build_descriptor_write(cb->second.device_id, writes[i], write_storage[i], vk_writes[i]))
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        dev->second.cmd_push_descriptor_set(cb->second.handle, static_cast<VkPipelineBindPoint>(bind_point), layout->second.handle, set,
                                            static_cast<uint32_t>(vk_writes.size()), vk_writes.data());
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

    int32_t vulkan_host::cmd_set_sample_mask(uint64_t command_buffer, uint32_t samples, const std::array<uint32_t, 2>& mask)
    {
        const auto cb = this->impl_->command_buffers.find(command_buffer);
        if (cb == this->impl_->command_buffers.end())
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        const auto dev = this->impl_->devices.find(cb->second.device_id);
        if (dev == this->impl_->devices.end() || !dev->second.cmd_set_sample_mask)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        dev->second.cmd_set_sample_mask(cb->second.handle, static_cast<VkSampleCountFlagBits>(samples), mask.data());
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
        case gpu_bridge::dynamic_state_u32::alpha_to_coverage_enable:
            if (!dev->second.cmd_set_alpha_to_coverage_enable)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_alpha_to_coverage_enable(handle, value);
            break;
        case gpu_bridge::dynamic_state_u32::rasterization_samples:
            if (!dev->second.cmd_set_rasterization_samples)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_rasterization_samples(handle, static_cast<VkSampleCountFlagBits>(value));
            break;
        case gpu_bridge::dynamic_state_u32::line_rasterization_mode:
            if (!dev->second.cmd_set_line_rasterization_mode)
            {
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            dev->second.cmd_set_line_rasterization_mode(handle, static_cast<VkLineRasterizationModeEXT>(value));
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
        constexpr uint32_t snapshot_version = 18;

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

            snapshot_transfer(const Device& device, const uint32_t queue_family,
                              const std::function<uint32_t(uint32_t type_bits)>& find_host_memory_type)
                : device_(device),
                  queue_family_(queue_family)
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

            // Multisampled images cannot be copied to buffers. A compute shader reads every sample as raw bits into the
            // staging buffer instead, one block of rows at a time; each sample is written back by drawing with a sample
            // mask that selects it, and stencil one bit at a time. Shaders can change the bits of some formats, so a
            // written image is read again and must match.
            static constexpr uint64_t sample_size = 16;

            bool read_samples(const VkImage image, const uint32_t format, const uint32_t samples, const VkImageAspectFlagBits aspect,
                              const VkImageAspectFlags barrier_aspects, const uint32_t layer, const VkExtent2D extent,
                              const VkImageLayout layout, const std::span<std::byte> contents)
            {
                const auto& device = this->device_;
                const uint64_t row_size = static_cast<uint64_t>(extent.width) * samples * sample_size;
                if (row_size == 0 || row_size > chunk_size || contents.size() != row_size * extent.height)
                {
                    return false;
                }

                const VkPipeline pipeline = this->read_pipeline(sample_variant(format, aspect));
                const VkImageView view = this->create_view(image, format, aspect, layer);
                const auto destroy_view = utils::finally([&] { device.destroy_image_view(device.handle, view, nullptr); });

                const VkImageLayout read_layout = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                                      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                this->bind_sample_resources(view, read_layout);

                const uint64_t rows_per_chunk = chunk_size / row_size;
                for (uint32_t row = 0; row < extent.height;)
                {
                    const auto rows = static_cast<uint32_t>(std::min<uint64_t>(rows_per_chunk, extent.height - row));
                    const bool first = row == 0;
                    const bool last = row + rows == extent.height;

                    this->submit([&](const VkCommandBuffer command_buffer) {
                        if (first)
                        {
                            this->image_barrier(command_buffer, image, barrier_aspects, layer, layout, read_layout);
                        }

                        const sample_parameters parameters{.width = extent.width, .rows = rows, .sample_count = samples, .first_row = row};
                        device.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                        device.cmd_bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, this->sample_pipeline_layout_, 0, 1,
                                                        &this->sample_descriptor_set_, 0, nullptr);
                        device.cmd_push_constants(command_buffer, this->sample_pipeline_layout_,
                                                  VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters),
                                                  &parameters);
                        device.cmd_dispatch(command_buffer,
                                            static_cast<uint32_t>((static_cast<uint64_t>(extent.width) * rows * samples + 63) / 64), 1, 1);

                        VkMemoryBarrier written{};
                        written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                        written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        written.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                        device.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                                    &written, 0, nullptr, 0, nullptr);

                        if (last)
                        {
                            this->image_barrier(command_buffer, image, barrier_aspects, layer, read_layout, layout);
                        }
                    });

                    std::memcpy(contents.data() + (row * row_size), this->staging_data_, static_cast<size_t>(rows * row_size));
                    row += rows;
                }

                return true;
            }

            // The image is in from, undefined for its first aspect, and only goes to layout after the last one.
            bool write_samples(const VkImage image, const uint32_t format, const uint32_t samples, const VkImageAspectFlagBits aspect,
                               const VkImageAspectFlags barrier_aspects, const uint32_t layer, const VkExtent2D extent, const bool first,
                               const bool last, const VkImageLayout layout, const std::span<std::byte> contents)
            {
                const auto& device = this->device_;
                const uint64_t row_size = static_cast<uint64_t>(extent.width) * samples * sample_size;
                if (row_size == 0 || row_size > chunk_size || contents.size() != row_size * extent.height)
                {
                    return false;
                }

                const bool depth_stencil = aspect != VK_IMAGE_ASPECT_COLOR_BIT;
                const VkImageLayout attachment_layout =
                    depth_stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                const auto variant = sample_variant(format, aspect);
                const VkRenderPass render_pass = this->sample_render_pass(format, samples, depth_stencil);

                const VkImageView view = this->create_view(image, format, barrier_aspects, layer);
                const auto destroy_view = utils::finally([&] { device.destroy_image_view(device.handle, view, nullptr); });

                VkFramebufferCreateInfo framebuffer_info{};
                framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer_info.renderPass = render_pass;
                framebuffer_info.attachmentCount = 1;
                framebuffer_info.pAttachments = &view;
                framebuffer_info.width = extent.width;
                framebuffer_info.height = extent.height;
                framebuffer_info.layers = 1;

                VkFramebuffer framebuffer{};
                if (device.create_framebuffer(device.handle, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating a framebuffer to restore a multisampled Vulkan image failed");
                }
                const auto destroy_framebuffer = utils::finally([&] { device.destroy_framebuffer(device.handle, framebuffer, nullptr); });

                this->bind_sample_resources(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);

                if (first)
                {
                    this->submit([&](const VkCommandBuffer command_buffer) {
                        if (!depth_stencil)
                        {
                            this->image_barrier(command_buffer, image, barrier_aspects, layer, VK_IMAGE_LAYOUT_UNDEFINED,
                                                attachment_layout);
                            return;
                        }

                        // Stencil is built up bit by bit, so it starts out cleared.
                        this->image_barrier(command_buffer, image, barrier_aspects, layer, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        const VkClearDepthStencilValue clear{.depth = 0.0F, .stencil = 0};
                        const VkImageSubresourceRange range{
                            .aspectMask = barrier_aspects, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1};
                        device.cmd_clear_depth_stencil_image(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                                             &range);
                        this->image_barrier(command_buffer, image, barrier_aspects, layer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            attachment_layout);
                    });
                }

                const uint64_t rows_per_chunk = chunk_size / row_size;
                for (uint32_t row = 0; row < extent.height;)
                {
                    const auto rows = static_cast<uint32_t>(std::min<uint64_t>(rows_per_chunk, extent.height - row));
                    std::memcpy(this->staging_data_, contents.data() + (row * row_size), static_cast<size_t>(rows * row_size));

                    this->submit([&](const VkCommandBuffer command_buffer) {
                        VkRenderPassBeginInfo begin{};
                        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                        begin.renderPass = render_pass;
                        begin.framebuffer = framebuffer;
                        begin.renderArea = {.offset = {.x = 0, .y = static_cast<int32_t>(row)},
                                            .extent = {.width = extent.width, .height = rows}};
                        device.cmd_begin_render_pass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);

                        const VkViewport viewport{.x = 0.0F,
                                                  .y = 0.0F,
                                                  .width = static_cast<float>(extent.width),
                                                  .height = static_cast<float>(extent.height),
                                                  .minDepth = 0.0F,
                                                  .maxDepth = 1.0F};
                        device.cmd_set_viewport(command_buffer, 0, 1, &viewport);
                        device.cmd_set_scissor(command_buffer, 0, 1, &begin.renderArea);
                        device.cmd_bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->sample_pipeline_layout_, 0,
                                                        1, &this->sample_descriptor_set_, 0, nullptr);

                        for (uint32_t sample = 0; sample < samples; ++sample)
                        {
                            device.cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                     this->write_pipeline(render_pass, format, samples, variant, sample));

                            const uint32_t bits = variant == sample_shader::stencil ? 8 : 1;
                            for (uint32_t bit = 0; bit < bits; ++bit)
                            {
                                const sample_parameters parameters{.width = extent.width,
                                                                   .rows = rows,
                                                                   .sample_count = samples,
                                                                   .first_row = row,
                                                                   .sample_index = sample,
                                                                   .stencil_bit = bit};
                                device.cmd_push_constants(command_buffer, this->sample_pipeline_layout_,
                                                          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters),
                                                          &parameters);
                                if (variant == sample_shader::stencil)
                                {
                                    device.cmd_set_stencil_write_mask(command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 1U << bit);
                                    device.cmd_set_stencil_reference(command_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
                                }
                                device.cmd_draw(command_buffer, 3, 1, 0, 0);
                            }
                        }

                        device.cmd_end_render_pass(command_buffer);
                    });

                    row += rows;
                }

                std::vector<std::byte> written(contents.size());
                if (!this->read_samples(image, format, samples, aspect, barrier_aspects, layer, extent, attachment_layout, written) ||
                    written != std::vector<std::byte>(contents.begin(), contents.end()))
                {
                    return false;
                }

                if (last)
                {
                    this->submit([&](const VkCommandBuffer command_buffer) {
                        this->image_barrier(command_buffer, image, barrier_aspects, layer, attachment_layout, layout);
                    });
                }

                return true;
            }

            void reset_query_pool(const VkQueryPool pool, const uint32_t count)
            {
                this->submit(
                    [&](const VkCommandBuffer command_buffer) { this->device_.cmd_reset_query_pool(command_buffer, pool, 0, count); });
            }

          private:
            enum class sample_shader : uint8_t
            {
                floating,
                unsigned_integer,
                signed_integer,
                depth,
                stencil,
            };

            struct sample_parameters
            {
                uint32_t width{};
                uint32_t rows{};
                uint32_t sample_count{};
                uint32_t layer{};
                uint32_t first_row{};
                uint32_t sample_index{};
                uint32_t stencil_bit{};
            };

            const Device& device_;
            uint32_t queue_family_{};
            VkQueue queue_{};
            std::unordered_map<std::string_view, VkShaderModule> shader_modules_{};
            VkDescriptorSetLayout sample_set_layout_{};
            VkPipelineLayout sample_pipeline_layout_{};
            VkDescriptorPool sample_descriptor_pool_{};
            VkDescriptorSet sample_descriptor_set_{};
            std::map<std::tuple<uint32_t, uint32_t, bool>, VkRenderPass> sample_render_passes_{};
            std::map<std::tuple<VkRenderPass, uint32_t, uint32_t, sample_shader, uint32_t>, VkPipeline> sample_pipelines_{};

            static sample_shader sample_variant(const uint32_t format, const VkImageAspectFlagBits aspect)
            {
                if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
                {
                    return sample_shader::depth;
                }

                if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT)
                {
                    return sample_shader::stencil;
                }

                switch (get_vulkan_format_numeric(format))
                {
                case vulkan_format_numeric::unsigned_integer:
                    return sample_shader::unsigned_integer;
                case vulkan_format_numeric::signed_integer:
                    return sample_shader::signed_integer;
                default:
                    return sample_shader::floating;
                }
            }

            VkShaderModule shader_module(const std::string_view name, const std::span<const uint32_t> code)
            {
                auto& module = this->shader_modules_[name];
                if (!module)
                {
                    VkShaderModuleCreateInfo info{};
                    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                    info.codeSize = code.size_bytes();
                    info.pCode = code.data();
                    if (this->device_.create_shader_module(this->device_.handle, &info, nullptr, &module) != VK_SUCCESS)
                    {
                        throw std::runtime_error("Creating a Vulkan snapshot shader failed");
                    }
                }
                return module;
            }

            void create_sample_layouts()
            {
                if (this->sample_pipeline_layout_)
                {
                    return;
                }

                const auto& device = this->device_;
                const std::array bindings{
                    VkDescriptorSetLayoutBinding{.binding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .pImmutableSamplers = nullptr},
                    VkDescriptorSetLayoutBinding{.binding = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                 .pImmutableSamplers = nullptr},
                };

                VkDescriptorSetLayoutCreateInfo set_layout_info{};
                set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                set_layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
                set_layout_info.pBindings = bindings.data();

                const VkPushConstantRange push_constants{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                         .offset = 0,
                                                         .size = sizeof(sample_parameters)};
                VkPipelineLayoutCreateInfo pipeline_layout_info{};
                pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                pipeline_layout_info.setLayoutCount = 1;
                pipeline_layout_info.pSetLayouts = &this->sample_set_layout_;
                pipeline_layout_info.pushConstantRangeCount = 1;
                pipeline_layout_info.pPushConstantRanges = &push_constants;

                const std::array sizes{
                    VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1},
                    VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1},
                };
                VkDescriptorPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                pool_info.maxSets = 1;
                pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
                pool_info.pPoolSizes = sizes.data();

                VkDescriptorSetAllocateInfo set_info{};
                set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                set_info.descriptorSetCount = 1;
                set_info.pSetLayouts = &this->sample_set_layout_;

                if (device.create_descriptor_set_layout(device.handle, &set_layout_info, nullptr, &this->sample_set_layout_) !=
                        VK_SUCCESS ||
                    device.create_pipeline_layout(device.handle, &pipeline_layout_info, nullptr, &this->sample_pipeline_layout_) !=
                        VK_SUCCESS ||
                    device.create_descriptor_pool(device.handle, &pool_info, nullptr, &this->sample_descriptor_pool_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot shader layouts failed");
                }

                set_info.descriptorPool = this->sample_descriptor_pool_;
                if (device.allocate_descriptor_sets(device.handle, &set_info, &this->sample_descriptor_set_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot shader layouts failed");
                }
            }

            void bind_sample_resources(const VkImageView view, const VkImageLayout layout)
            {
                this->create_sample_layouts();

                const VkDescriptorImageInfo image_info{.sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = layout};
                const VkDescriptorBufferInfo buffer_info{.buffer = this->staging_buffer_, .offset = 0, .range = VK_WHOLE_SIZE};
                std::array<VkWriteDescriptorSet, 2> writes{};
                for (auto& write : writes)
                {
                    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    write.dstSet = this->sample_descriptor_set_;
                    write.descriptorCount = 1;
                }
                writes[0].dstBinding = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[0].pBufferInfo = &buffer_info;
                writes[1].dstBinding = 0;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[1].pImageInfo = &image_info;

                this->device_.update_descriptor_sets(this->device_.handle, view ? 2 : 1, writes.data(), 0, nullptr);
            }

            VkPipeline read_pipeline(const sample_shader variant)
            {
                this->create_sample_layouts();

                const auto read_variant = [&] {
                    switch (variant)
                    {
                    case sample_shader::stencil:
                        return sample_shader::unsigned_integer;
                    case sample_shader::depth:
                        return sample_shader::floating;
                    default:
                        return variant;
                    }
                }();
                auto& pipeline = this->sample_pipelines_[{VK_NULL_HANDLE, 0, 0, read_variant, 0}];
                if (pipeline)
                {
                    return pipeline;
                }

                const VkShaderModule module = [&] {
                    switch (read_variant)
                    {
                    case sample_shader::unsigned_integer:
                        return this->shader_module("read_samples_uint", snapshot_shaders::read_samples_uint);
                    case sample_shader::signed_integer:
                        return this->shader_module("read_samples_sint", snapshot_shaders::read_samples_sint);
                    default:
                        return this->shader_module("read_samples_float", snapshot_shaders::read_samples_float);
                    }
                }();

                const VkComputePipelineCreateInfo info{
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                              .pNext = nullptr,
                              .flags = 0,
                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                              .module = module,
                              .pName = "main",
                              .pSpecializationInfo = nullptr},
                    .layout = this->sample_pipeline_layout_,
                    .basePipelineHandle = VK_NULL_HANDLE,
                    .basePipelineIndex = -1,
                };

                if (this->device_.create_compute_pipelines(this->device_.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
                    VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot read pipeline failed");
                }
                return pipeline;
            }

            VkRenderPass sample_render_pass(const uint32_t format, const uint32_t samples, const bool depth_stencil)
            {
                auto& render_pass = this->sample_render_passes_[{format, samples, depth_stencil}];
                if (render_pass)
                {
                    return render_pass;
                }

                const VkImageLayout layout =
                    depth_stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                const VkAttachmentDescription attachment{.flags = 0,
                                                         .format = static_cast<VkFormat>(format),
                                                         .samples = static_cast<VkSampleCountFlagBits>(samples),
                                                         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                         .initialLayout = layout,
                                                         .finalLayout = layout};
                const VkAttachmentReference reference{.attachment = 0, .layout = layout};

                VkSubpassDescription subpass{};
                subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                subpass.colorAttachmentCount = depth_stencil ? 0 : 1;
                subpass.pColorAttachments = depth_stencil ? nullptr : &reference;
                subpass.pDepthStencilAttachment = depth_stencil ? &reference : nullptr;

                VkRenderPassCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
                info.attachmentCount = 1;
                info.pAttachments = &attachment;
                info.subpassCount = 1;
                info.pSubpasses = &subpass;

                if (this->device_.create_render_pass(this->device_.handle, &info, nullptr, &render_pass) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot render pass failed");
                }
                return render_pass;
            }

            VkPipeline write_pipeline(const VkRenderPass render_pass, const uint32_t format, const uint32_t samples,
                                      const sample_shader variant, const uint32_t sample)
            {
                auto& pipeline = this->sample_pipelines_[{render_pass, format, samples, variant, sample}];
                if (pipeline)
                {
                    return pipeline;
                }

                const VkShaderModule fragment = [&] {
                    switch (variant)
                    {
                    case sample_shader::unsigned_integer:
                        return this->shader_module("write_sample_uint", snapshot_shaders::write_sample_uint);
                    case sample_shader::signed_integer:
                        return this->shader_module("write_sample_sint", snapshot_shaders::write_sample_sint);
                    case sample_shader::depth:
                        return this->shader_module("write_sample_depth", snapshot_shaders::write_sample_depth);
                    case sample_shader::stencil:
                        return this->shader_module("write_sample_stencil", snapshot_shaders::write_sample_stencil);
                    default:
                        return this->shader_module("write_sample_float", snapshot_shaders::write_sample_float);
                    }
                }();

                const auto shader_stage = [](const VkShaderStageFlagBits stage, const VkShaderModule module) {
                    return VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                           .pNext = nullptr,
                                                           .flags = 0,
                                                           .stage = stage,
                                                           .module = module,
                                                           .pName = "main",
                                                           .pSpecializationInfo = nullptr};
                };
                const std::array stages{
                    shader_stage(VK_SHADER_STAGE_VERTEX_BIT, this->shader_module("fullscreen", snapshot_shaders::fullscreen)),
                    shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment),
                };

                VkPipelineVertexInputStateCreateInfo vertex_input{};
                vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

                VkPipelineInputAssemblyStateCreateInfo input_assembly{};
                input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

                VkPipelineViewportStateCreateInfo viewport{};
                viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                viewport.viewportCount = 1;
                viewport.scissorCount = 1;

                VkPipelineRasterizationStateCreateInfo rasterization{};
                rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rasterization.polygonMode = VK_POLYGON_MODE_FILL;
                rasterization.cullMode = VK_CULL_MODE_NONE;
                rasterization.lineWidth = 1.0F;

                const std::array<VkSampleMask, 2> sample_mask{sample < 32 ? 1U << sample : 0U, sample >= 32 ? 1U << (sample - 32) : 0U};
                const VkPipelineMultisampleStateCreateInfo multisample{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples),
                    .sampleShadingEnable = VK_FALSE,
                    .minSampleShading = 0.0F,
                    .pSampleMask = sample_mask.data(),
                    .alphaToCoverageEnable = VK_FALSE,
                    .alphaToOneEnable = VK_FALSE,
                };

                VkPipelineDepthStencilStateCreateInfo depth_stencil{};
                depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                depth_stencil.depthTestEnable = variant == sample_shader::depth ? VK_TRUE : VK_FALSE;
                depth_stencil.depthWriteEnable = variant == sample_shader::depth ? VK_TRUE : VK_FALSE;
                depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
                depth_stencil.stencilTestEnable = variant == sample_shader::stencil ? VK_TRUE : VK_FALSE;
                depth_stencil.front = {.failOp = VK_STENCIL_OP_KEEP,
                                       .passOp = VK_STENCIL_OP_REPLACE,
                                       .depthFailOp = VK_STENCIL_OP_KEEP,
                                       .compareOp = VK_COMPARE_OP_ALWAYS,
                                       .compareMask = 0xFF,
                                       .writeMask = 0xFF,
                                       .reference = 0xFF};
                depth_stencil.back = depth_stencil.front;

                const VkPipelineColorBlendAttachmentState blend_attachment{.blendEnable = VK_FALSE,
                                                                           .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                           .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                           .colorBlendOp = VK_BLEND_OP_ADD,
                                                                           .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                           .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                           .alphaBlendOp = VK_BLEND_OP_ADD,
                                                                           .colorWriteMask =
                                                                               VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
                const bool color = variant != sample_shader::depth && variant != sample_shader::stencil;
                VkPipelineColorBlendStateCreateInfo blend{};
                blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                blend.attachmentCount = color ? 1 : 0;
                blend.pAttachments = &blend_attachment;

                const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
                dynamic.pDynamicStates = dynamic_states.data();

                VkGraphicsPipelineCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                info.stageCount = static_cast<uint32_t>(stages.size());
                info.pStages = stages.data();
                info.pVertexInputState = &vertex_input;
                info.pInputAssemblyState = &input_assembly;
                info.pViewportState = &viewport;
                info.pRasterizationState = &rasterization;
                info.pMultisampleState = &multisample;
                info.pDepthStencilState = color ? nullptr : &depth_stencil;
                info.pColorBlendState = &blend;
                info.pDynamicState = &dynamic;
                info.layout = this->sample_pipeline_layout_;
                info.renderPass = render_pass;

                if (this->device_.create_graphics_pipelines(this->device_.handle, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
                    VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot write pipeline failed");
                }
                return pipeline;
            }

            VkImageView create_view(const VkImage image, const uint32_t format, const VkImageAspectFlags aspects,
                                    const uint32_t layer) const
            {
                VkImageViewCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                info.image = image;
                info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                info.format = static_cast<VkFormat>(format);
                info.subresourceRange = {
                    .aspectMask = aspects, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1};

                VkImageView view{};
                if (this->device_.create_image_view(this->device_.handle, &info, nullptr, &view) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating a view of a multisampled Vulkan image failed");
                }
                return view;
            }

            void image_barrier(const VkCommandBuffer command_buffer, const VkImage image, const VkImageAspectFlags aspects,
                               const uint32_t layer, const VkImageLayout from, const VkImageLayout to) const
            {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.oldLayout = from;
                barrier.newLayout = to;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {
                    .aspectMask = aspects, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = layer, .layerCount = 1};
                this->device_.cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &barrier);
            }

            VkCommandPool pool_{};
            VkCommandBuffer command_buffer_{};
            VkBuffer staging_buffer_{};
            VkDeviceMemory staging_memory_{};
            void* staging_data_{};

            void create(const std::function<uint32_t(uint32_t type_bits)>& find_host_memory_type)
            {
                const auto& device = this->device_;
                device.get_device_queue(device.handle, this->queue_family_, 0, &this->queue_);

                VkCommandPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_info.queueFamilyIndex = this->queue_family_;

                VkCommandBufferAllocateInfo command_buffer_info{};
                command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                command_buffer_info.commandBufferCount = 1;

                if (!this->queue_ || device.create_command_pool(device.handle, &pool_info, nullptr, &this->pool_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot transfer queue failed");
                }

                command_buffer_info.commandPool = this->pool_;
                if (device.allocate_command_buffers(device.handle, &command_buffer_info, &this->command_buffer_) != VK_SUCCESS)
                {
                    throw std::runtime_error("Creating the Vulkan snapshot transfer queue failed");
                }

                this->staging_buffer_ = this->create_transfer_buffer(chunk_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
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
                for (const auto& [key, pipeline] : this->sample_pipelines_)
                {
                    device.destroy_pipeline(device.handle, pipeline, nullptr);
                }
                for (const auto& [key, render_pass] : this->sample_render_passes_)
                {
                    device.destroy_render_pass(device.handle, render_pass, nullptr);
                }
                for (const auto& [name, module] : this->shader_modules_)
                {
                    device.destroy_shader_module(device.handle, module, nullptr);
                }
                if (this->sample_pipeline_layout_)
                {
                    device.destroy_pipeline_layout(device.handle, this->sample_pipeline_layout_, nullptr);
                }
                if (this->sample_set_layout_)
                {
                    device.destroy_descriptor_set_layout(device.handle, this->sample_set_layout_, nullptr);
                }
                if (this->sample_descriptor_pool_)
                {
                    device.destroy_descriptor_pool(device.handle, this->sample_descriptor_pool_, nullptr);
                }
                this->sample_pipelines_.clear();
                this->sample_render_passes_.clear();
                this->shader_modules_.clear();
                this->sample_pipeline_layout_ = VK_NULL_HANDLE;
                this->sample_set_layout_ = VK_NULL_HANDLE;
                this->sample_descriptor_pool_ = VK_NULL_HANDLE;

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

            VkBuffer create_transfer_buffer(const VkDeviceSize size, const VkBufferUsageFlags extra_usage = 0) const
            {
                VkBufferCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                info.size = size;
                info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra_usage;
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

    void vulkan_host::quiesce()
    {
        auto& impl = *this->impl_;

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

        if (impl.has_pending_submissions())
        {
            throw std::runtime_error("A Vulkan queue waits for something no submission, acquire or host signal provides");
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

        impl.quiesced = true;
    }

    void vulkan_host::serialize(utils::buffer_serializer& buffer) const
    {
        auto& impl = *this->impl_;
        if (!impl.quiesced)
        {
            throw std::runtime_error("Vulkan state can only be saved after the GPU finished its work");
        }

        std::map<std::pair<uint64_t, uint32_t>, std::unique_ptr<snapshot_transfer<impl::device_data>>> transfers{};
        const auto transfer_for = [&](const uint64_t device_id,
                                      const uint32_t family = VK_QUEUE_FAMILY_IGNORED) -> snapshot_transfer<impl::device_data>& {
            const auto& device = impl.devices.at(device_id);
            const auto queue_family = family == VK_QUEUE_FAMILY_IGNORED ? device.queue_family_index : family;
            auto& transfer = transfers[{device_id, queue_family}];
            if (!transfer)
            {
                transfer = std::make_unique<snapshot_transfer<impl::device_data>>(device, queue_family, [&](const uint32_t type_bits) {
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

            const auto index = impl.substitute_cached_memory_type(device, device.memory_types.to_host(memory_type_index));
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
            buffer.write_span(std::span(device.memory_types.properties.memoryTypes, device.memory_types.properties.memoryTypeCount));
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
            if (!device.memory_types.is_host_visible(memory.memory_type_index) ||
                (property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            {
                std::vector<std::byte> contents(static_cast<size_t>(memory.allocation_size));
                if (!transfer_for(memory.device_id)
                         .copy(memory.handle,
                               impl.substitute_cached_memory_type(device, device.memory_types.to_host(memory.memory_type_index)), contents,
                               false))
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

            buffer.write_span(std::span(static_cast<const std::byte*>(mapped), static_cast<size_t>(memory.allocation_size)));

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
            buffer.write_vector(image.owners);

            if (image.ownership_unknown ||
                std::ranges::any_of(image.released_to, [](const uint32_t family) { return family != VK_QUEUE_FAMILY_IGNORED; }))
            {
                throw std::runtime_error("Vulkan image " + std::to_string(id) + " is being handed between queue families");
            }

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

                        const auto first_aspect = image.subresource_index(0, mip, layer);
                        const auto owner = image.owners[first_aspect];
                        const bool layouts_match = aspect == 0 || (layout == static_cast<VkImageLayout>(image.layouts[first_aspect]) &&
                                                                   owner == image.owners[image.subresource_index(aspect, mip, layer)]);
                        // Multisampled contents are read and written by shaders, which the device's first queue family runs.
                        const bool owner_runs_shaders = image.samples == 1 || owner == VK_QUEUE_FAMILY_IGNORED ||
                                                        owner == impl.devices.at(image.device_id).queue_family_index;
                        if (!image.copyable || !layouts_match || !owner_runs_shaders)
                        {
                            throw std::runtime_error("Snapshots cannot save the contents of Vulkan image " + std::to_string(id) + " yet");
                        }

                        if (image.samples > 1)
                        {
                            const VkExtent2D extent{.width = create.width, .height = create.height};
                            std::vector<std::byte> contents(static_cast<size_t>(extent.width) * extent.height * image.samples *
                                                            snapshot_transfer<impl::device_data>::sample_size);
                            if (!transfer_for(image.device_id, owner)
                                     .read_samples(image.handle, create.format, image.samples, static_cast<VkImageAspectFlagBits>(bit),
                                                   aspects, layer, extent, layout, contents))
                            {
                                throw std::runtime_error("Snapshots cannot save the contents of Vulkan image " + std::to_string(id) +
                                                         " yet");
                            }

                            buffer.write(true);
                            buffer.write(bit);
                            buffer.write(mip);
                            buffer.write(layer);
                            buffer.write_vector(contents);
                            continue;
                        }

                        const auto block = get_vulkan_copy_block(create.format, bit);
                        if (!block)
                        {
                            throw std::runtime_error("Snapshots cannot save the contents of Vulkan image " + std::to_string(id) + " yet");
                        }

                        const VkExtent3D extent{.width = std::max(1U, create.width >> mip),
                                                .height = std::max(1U, create.height >> mip),
                                                .depth = std::max(1U, create.depth >> mip)};
                        const uint64_t size = ((extent.width + block->width - 1) / block->width) *
                                              ((extent.height + block->height - 1) / block->height) * extent.depth * block->size;

                        std::vector<std::byte> contents(static_cast<size_t>(size));
                        if (!transfer_for(image.device_id, owner)
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
            // A held proxy image is only bookkeeping and comes back as such, but a held native image cannot be acquired again
            // by index.
            if (!swapchain.proxy && std::ranges::any_of(swapchain.acquired, [](const bool acquired) { return acquired; }))
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
            buffer.write(swapchain.proxy.has_value());
            buffer.write_vector(swapchain.image_ids);
            for (const auto image : swapchain.image_ids)
            {
                const auto& data = impl.images.at(image);
                buffer.write_vector(data.layouts);
                buffer.write(data.last_queue_id);
            }
            buffer.write_vector(std::vector<uint8_t>(swapchain.acquired.begin(), swapchain.acquired.end()));

            // A held proxy image may hold a frame the guest has not presented yet.
            for (size_t i = 0; i < swapchain.image_ids.size(); ++i)
            {
                const bool held = swapchain.proxy && swapchain.acquired[i];
                buffer.write(held);
                if (!held)
                {
                    continue;
                }

                const auto& data = impl.images.at(swapchain.image_ids[i]);
                const auto block = get_vulkan_copy_block(data.create.format, VK_IMAGE_ASPECT_COLOR_BIT);
                const VkExtent3D extent{.width = data.create.width, .height = data.create.height, .depth = 1};
                for (uint32_t layer = 0; layer < data.create.array_layers; ++layer)
                {
                    const auto layout = data.host_layout(data.layouts[data.subresource_index(0, 0, layer)]);
                    buffer.write(layout != VK_IMAGE_LAYOUT_UNDEFINED);
                    if (layout == VK_IMAGE_LAYOUT_UNDEFINED)
                    {
                        continue;
                    }

                    std::vector<std::byte> contents{};
                    if (block)
                    {
                        contents.resize(static_cast<size_t>(((extent.width + block->width - 1) / block->width) *
                                                            ((extent.height + block->height - 1) / block->height) * block->size));
                    }
                    if (!block || !transfer_for(data.device_id)
                                       .copy_image(data.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, extent,
                                                   *block, layout, layout, true, contents, false))
                    {
                        throw std::runtime_error("Snapshots cannot save the contents of a held Vulkan swapchain image yet");
                    }
                    buffer.write_vector(contents);
                }
            }
        }

        buffer.write(static_cast<uint64_t>(impl.query_pools.size()));
        for (const auto& [id, pool] : impl.query_pools)
        {
            if (!pool.snapshot_supported())
            {
                throw std::runtime_error("Snapshots do not support Vulkan query pools of type " + std::to_string(pool.query_type) + " yet");
            }

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

            const auto absent = [](const impl::shader_stage_params& stage) {
                return stage.module == 0 && stage.identifier.empty() && stage.code == nullptr;
            };

            if (!restorable(pipeline.create.vertex) ||
                (!pipeline.create.compute && !absent(pipeline.create.fragment) && !restorable(pipeline.create.fragment)))
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

        std::map<std::pair<uint64_t, uint32_t>, std::unique_ptr<snapshot_transfer<impl::device_data>>> transfers{};
        const auto transfer_for = [&](const uint64_t device_id,
                                      const uint32_t family = VK_QUEUE_FAMILY_IGNORED) -> snapshot_transfer<impl::device_data>& {
            const auto& device = impl.devices.at(device_id);
            const auto queue_family = family == VK_QUEUE_FAMILY_IGNORED ? device.queue_family_index : family;
            auto& transfer = transfers[{device_id, queue_family}];
            if (!transfer)
            {
                transfer = std::make_unique<snapshot_transfer<impl::device_data>>(device, queue_family, [&](const uint32_t type_bits) {
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
            for (const auto& destroy : std::views::reverse(temporaries))
            {
                destroy();
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
            ids.reserve(create.set_layouts.size());
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
            if (this->create_render_pass(device, create.description, render_pass) != VK_SUCCESS)
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
            std::vector<std::string> extensions(static_cast<size_t>(buffer.read<uint64_t>()));
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

            const auto& physical_device = impl.physical_devices.at(create.physical_device);
            const auto& instance = impl.instances.at(physical_device.instance_id);
            uint32_t family_count = 0;
            instance.get_queue_family_properties(physical_device.handle, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            instance.get_queue_family_properties(physical_device.handle, &family_count, families.data());
            if (std::ranges::any_of(create.queues, [&](const auto& queue) {
                    return queue.queue_family_index >= family_count || queue.queue_count > families[queue.queue_family_index].queueCount;
                }))
            {
                throw std::runtime_error("This GPU no longer offers the queues of Vulkan device " + std::to_string(id));
            }

            uint64_t device{};
            impl.forced_ids = {id};
            const auto result =
                this->create_device(create.physical_device, create.queues.data(), create.queues.size(), create.extensions.data(),
                                    create.extensions.size(), create.extension_count, create.features.data(), create.features.size(),
                                    create.feature_count, create.spoof_missing_features, device);
            expect_restored(result, device, id, "device");

            const auto& restored_types = impl.devices.at(id).memory_types.properties;
            const auto saved_types = buffer.read_vector<VkMemoryType>();
            if (saved_types.size() != restored_types.memoryTypeCount ||
                !std::equal(saved_types.begin(), saved_types.end(), restored_types.memoryTypes,
                            [](const VkMemoryType& a, const VkMemoryType& b) {
                                return a.propertyFlags == b.propertyFlags && a.heapIndex == b.heapIndex;
                            }))
            {
                throw std::runtime_error("This GPU no longer offers the memory types of Vulkan device " + std::to_string(id));
            }
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

            const auto memory_type = impl.substitute_cached_memory_type(dev, dev.memory_types.to_host(memory_type_index));
            VkPhysicalDeviceMemoryProperties host_properties{};
            if (!dev.memory_types.is_host_visible(memory_type_index) || !impl.query_memory_properties(dev, host_properties) ||
                memory_type >= host_properties.memoryTypeCount ||
                (host_properties.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
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
            auto owners = buffer.read_vector<uint32_t>();

            uint64_t image{};
            impl.forced_ids = {id};
            const auto result =
                this->create_image(device, create.format, create.width, create.height, create.usage, create.tiling, create.samples,
                                   create.image_type, create.depth, create.mip_levels, create.array_layers, create.flags, image);
            expect_restored(result, image, id, "image");

            auto& restored = impl.images.at(id);
            if (layouts.size() != restored.layouts.size() || owners.size() != restored.owners.size() ||
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

                const auto saved_layout = static_cast<VkImageLayout>(layouts[restored.subresource_index(0, entry.mip, entry.layer)]);
                const auto owner_family = owners[restored.subresource_index(0, entry.mip, entry.layer)];
                if (restored.samples > 1)
                {
                    if (!transfer_for(device, owner_family)
                             .write_samples(restored.handle, create.format, restored.samples,
                                            static_cast<VkImageAspectFlagBits>(entry.aspect), aspects, entry.layer,
                                            {.width = create.width, .height = create.height}, first, last, saved_layout, entry.contents))
                    {
                        throw std::runtime_error("Restoring the samples of Vulkan image " + std::to_string(id) + " failed");
                    }
                    continue;
                }

                const auto block = get_vulkan_copy_block(create.format, entry.aspect);
                const VkExtent3D extent{.width = std::max(1U, create.width >> entry.mip),
                                        .height = std::max(1U, create.height >> entry.mip),
                                        .depth = std::max(1U, create.depth >> entry.mip)};
                if (!block || entry.mip >= create.mip_levels || entry.layer >= create.array_layers ||
                    !transfer_for(device, owner_family)
                         .copy_image(restored.handle, static_cast<VkImageAspectFlagBits>(entry.aspect), aspects, entry.mip, entry.layer,
                                     extent, *block, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     static_cast<VkImageLayout>(layouts[restored.subresource_index(0, entry.mip, entry.layer)]), last,
                                     entry.contents, true))
                {
                    throw std::runtime_error("Restoring the contents of Vulkan image " + std::to_string(id) + " failed");
                }
            }

            restored.layouts = std::move(layouts);
            restored.owners = std::move(owners);
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto instance = buffer.read<uint64_t>();

            uint64_t surface{};
            impl.forced_ids = {id};
            const auto native_surface = callbacks.create_native_surface(id, instance);
            int32_t result = VK_ERROR_SURFACE_LOST_KHR;
            if (native_surface != 0)
            {
                result = this->create_surface(instance, native_surface, surface);
            }
            else if (impl.instances.contains(instance))
            {
                // The guest may keep a surface after destroying its window, as Mesa's WGL backend does. It stays a valid
                // object that only reports VK_ERROR_SURFACE_LOST_KHR.
                surface = impl.allocate_id();
                impl.surfaces.emplace(surface, impl::surface_data{.handle = VK_NULL_HANDLE, .instance_id = instance});
                result = VK_SUCCESS;
            }
            expect_restored(result, surface, id, "surface");
        }

        for (auto count = buffer.read<uint64_t>(); count > 0; --count)
        {
            const auto id = buffer.read<uint64_t>();
            const auto device = buffer.read<uint64_t>();
            const auto surface = buffer.read<uint64_t>();
            const auto create = buffer.read<impl::swapchain_params>();
            const auto proxied = buffer.read<bool>();
            const auto image_ids = buffer.read_vector<uint64_t>();

            uint64_t swapchain{};
            uint32_t image_count = 0;
            impl.forced_ids.assign(image_ids.begin(), image_ids.end());
            impl.forced_ids.push_back(id);

            // A swapchain on a surface whose window is gone keeps its images, which views and descriptors may still refer
            // to. Without a native swapchain behind them, it only reports that it is out of date.
            if (const auto lost_surface = impl.surfaces.find(surface); lost_surface != impl.surfaces.end() && !lost_surface->second.handle)
            {
                const auto& dev = impl.devices.at(device);
                impl::swapchain_data sc{.device_id = device, .surface_id = surface, .create = create};
                sc.proxy.emplace();
                sc.proxy->lost = true;
                if (impl.create_swapchain_proxy(dev, impl::proxy_image_info(sc.create), static_cast<uint32_t>(image_ids.size()), {},
                                                *sc.proxy) != VK_SUCCESS)
                {
                    impl::destroy_swapchain_proxy(dev, *sc.proxy);
                    throw std::runtime_error("Restoring Vulkan swapchain " + std::to_string(id) + " of a lost surface failed");
                }
                impl.register_swapchain_images(sc, sc.proxy->images);
                swapchain = impl.allocate_id();
                impl.swapchains.emplace(swapchain, std::move(sc));
                expect_restored(VK_SUCCESS, swapchain, id, "swapchain");
            }
            else
            {
                const auto result = this->create_swapchain(
                    device, surface, 0, create.flags, create.format, create.color_space, create.width, create.height,
                    create.image_array_layers, create.min_image_count, create.image_usage, create.sharing_mode, create.queue_family_indices,
                    create.pre_transform, create.composite_alpha, create.present_mode, create.clipped, swapchain, image_count);
                if (result == VK_SUCCESS && image_count != image_ids.size())
                {
                    throw std::runtime_error("The re-created Vulkan swapchain has a different number of images");
                }
                expect_restored(result, swapchain, id, "swapchain");
                if (impl.swapchains.at(swapchain).proxy.has_value() != proxied)
                {
                    throw std::runtime_error("The host surface no longer presents the Vulkan swapchain the same way");
                }
            }

            for (const auto image : image_ids)
            {
                auto& restored = impl.images.at(image);
                auto layouts = buffer.read_vector<uint32_t>();
                if (layouts.size() != restored.layouts.size())
                {
                    throw std::runtime_error("The re-created Vulkan swapchain has different images");
                }
                restored.layouts = std::move(layouts);
                restored.last_queue_id = buffer.read<uint64_t>();
                restored.layout_undefined =
                    std::ranges::any_of(restored.layouts, [](const uint32_t layout) { return layout != VK_IMAGE_LAYOUT_UNDEFINED; });
            }

            auto& restored_swapchain = impl.swapchains.at(swapchain);
            const auto acquired = buffer.read_vector<uint8_t>();
            if (acquired.size() != restored_swapchain.acquired.size())
            {
                throw std::runtime_error("The re-created Vulkan swapchain has different images");
            }

            for (size_t i = 0; i < acquired.size(); ++i)
            {
                restored_swapchain.acquired[i] = acquired[i] != 0;
            }

            // A held image is not acquired again, which is where a re-created image otherwise gets its saved layout. Writing
            // the saved frame back puts it there.
            for (const auto image_id : image_ids)
            {
                if (!buffer.read<bool>())
                {
                    continue;
                }

                auto& image = impl.images.at(image_id);
                const auto block = get_vulkan_copy_block(image.create.format, VK_IMAGE_ASPECT_COLOR_BIT);
                const VkExtent3D extent{.width = image.create.width, .height = image.create.height, .depth = 1};
                for (uint32_t layer = 0; layer < image.create.array_layers; ++layer)
                {
                    if (!buffer.read<bool>())
                    {
                        continue;
                    }

                    auto contents = buffer.read_vector<std::byte>();
                    const auto layout = image.host_layout(image.layouts[image.subresource_index(0, 0, layer)]);
                    if (!block ||
                        !transfer_for(device).copy_image(image.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, layer,
                                                         extent, *block, VK_IMAGE_LAYOUT_UNDEFINED, layout, true, contents, true))
                    {
                        throw std::runtime_error("Restoring a held Vulkan swapchain image failed");
                    }
                }
                image.layout_undefined = false;
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
            const auto result = this->create_render_pass(device, create.description, render_pass);
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
                    device, cache, render_pass, create.subpass, layout, vertex, fragment, create.flags, create.width, create.height,
                    create.bindings, create.attributes, create.divisors, create.depth, create.color_formats, create.depth_format,
                    create.stencil_format, create.rasterization_samples, create.primitive_topology, create.primitive_restart_enable,
                    create.rasterization_stream, create.rasterization_stream_flags, create.dynamic_states,
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
            if (!std::ranges::all_of(create.views, [&](const uint64_t view) { return impl.image_views.contains(view); }))
            {
                continue;
            }

            const auto render_pass = ensure_render_pass(device, create.render_pass, create.render_pass_create);
            uint64_t framebuffer{};
            impl.forced_ids = {id};
            const auto result = this->create_framebuffer(device, render_pass, create.flags, create.width, create.height, create.layers,
                                                         create.views, create.images, framebuffer);
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

            std::vector<std::pair<uint64_t, impl::descriptor_slot>> slots(static_cast<size_t>(buffer.read<uint64_t>()));
            for (auto& [key, slot] : slots)
            {
                buffer.read(key);
                buffer.read(slot);
            }

            std::vector<std::pair<uint32_t, std::vector<std::byte>>> inline_blocks(static_cast<size_t>(buffer.read<uint64_t>()));
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
            writes.reserve(slots.size() + inline_blocks.size());
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
