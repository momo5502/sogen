#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sogen
{
    // Thin host-side wrapper around the real Vulkan driver. Owns the dynamically loaded loader and
    // the mapping from opaque object ids (handed to the guest) to live Vulkan handles, so raw host
    // pointers never cross the bridge.
    //
    // This interface is deliberately free of emulator/guest-Windows types: its implementation
    // includes the host <vulkan/vulkan_core.h> and the platform dynamic loader, which must not be
    // mixed with the emulated Windows definitions used elsewhere in windows-emulator. VkResult is
    // surfaced as int32_t for the same reason.
    class vulkan_host
    {
      public:
        vulkan_host();
        ~vulkan_host();

        vulkan_host(const vulkan_host&) = delete;
        vulkan_host& operator=(const vulkan_host&) = delete;
        vulkan_host(vulkan_host&&) = delete;
        vulkan_host& operator=(vulkan_host&&) = delete;

        // False if no Vulkan driver could be loaded on the host.
        bool available() const;

        // Creates a bare instance (no layers/extensions). out_instance is set to a fresh object id
        // on success, or 0 on failure.
        int32_t create_instance(uint64_t& out_instance);
        void destroy_instance(uint64_t instance);

        // Reports the host physical devices for the instance. out_count always receives the true
        // device count; up to out_devices.size() ids are written. Ids are stable across calls.
        int32_t enumerate_physical_devices(uint64_t instance, std::span<uint64_t> out_devices, uint32_t& out_count);

        // Writes up to out_size bytes of the device's VkPhysicalDeviceProperties into out. When
        // guest_is_32bit is set, the struct is repacked into the guest's 32-bit (WoW64) ABI layout.
        int32_t get_physical_device_properties(uint64_t physical_device, void* out, size_t out_size, bool guest_is_32bit);

        // Real VkFormatProperties feature flags for the given VkFormat.
        int32_t get_physical_device_format_properties(uint64_t physical_device, uint32_t format, uint32_t& out_linear,
                                                      uint32_t& out_optimal, uint32_t& out_buffer);

        // Real VkImageFormatProperties (incl. supported sampleCounts) for the image config.
        struct image_format_properties
        {
            uint32_t max_mip_levels;
            uint32_t max_array_layers;
            uint32_t sample_counts;
            uint32_t max_extent_width;
            uint32_t max_extent_height;
            uint32_t max_extent_depth;
            uint64_t max_resource_size;
        };
        int32_t get_physical_device_image_format_properties(uint64_t physical_device, uint32_t format, uint32_t type, uint32_t tiling,
                                                            uint32_t usage, uint32_t flags, image_format_properties& out);

        // Writes the device's queue families as raw VkQueueFamilyProperties into out (sized in
        // bytes). out_count always receives the true family count.
        int32_t get_queue_family_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count);

        // Writes the device's real VkExtensionProperties array into out (sized in bytes); out_count
        // always receives the true device-extension count.
        int32_t enumerate_device_extension_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count);

        // Queries vkGetPhysicalDeviceFeatures2 for the pNext chain the guest described. in_records is a
        // packed array of `struct_count` feature_chain_record {sType, body_size} (guest-supplied,
        // pad-free body sizes). out_blob receives, in the same order, one record + body bytes per entry
        // with the device's real feature values (body_size 0 for structs the host does not know).
        int32_t get_physical_device_features2(uint64_t physical_device, const void* in_records, size_t in_records_size,
                                              uint32_t struct_count, std::vector<std::byte>& out_blob);

        // Same chain convention as get_physical_device_features2, but for the property structs chained on
        // VkPhysicalDeviceProperties2 (e.g. VkPhysicalDeviceRobustness2PropertiesEXT). The base
        // VkPhysicalDeviceProperties is not included here (fetched via get_physical_device_properties).
        int32_t get_physical_device_properties2(uint64_t physical_device, const void* in_records, size_t in_records_size,
                                                uint32_t struct_count, std::vector<std::byte>& out_blob);

        // Creates a logical device with a single queue family (queue_count queues, default priority),
        // enabling the given device extensions (extension_blob = `extension_count` NUL-terminated names)
        // and features (feature_blob = `feature_struct_count` feature_chain_record + body entries, same
        // format as get_physical_device_features2). out_device receives a fresh object id, or 0 on failure.
        int32_t create_device(uint64_t physical_device, const void* queue_entries, size_t queue_entry_count, const void* extension_blob,
                              size_t extension_blob_size, uint32_t extension_count, const void* feature_blob, size_t feature_blob_size,
                              uint32_t feature_struct_count, uint64_t& out_device);
        void destroy_device(uint64_t device);

        // Resolves a queue created with the device; out_queue receives a stable object id.
        int32_t get_device_queue(uint64_t device, uint32_t queue_family_index, uint32_t queue_index, uint64_t& out_queue);

        int32_t create_command_pool(uint64_t device, uint32_t queue_family_index, uint32_t flags, uint64_t& out_pool);
        void destroy_command_pool(uint64_t device, uint64_t pool);

        // Allocates a single command buffer from the pool (level 0 = primary, 1 = secondary).
        int32_t allocate_command_buffer(uint64_t device, uint64_t pool, uint32_t level, uint64_t& out_command_buffer);
        void free_command_buffer(uint64_t device, uint64_t pool, uint64_t command_buffer);

        // is_secondary supplies the dynamic-rendering inheritance (attachment formats) the secondary renders to.
        int32_t begin_command_buffer(uint64_t command_buffer, uint32_t flags, bool is_secondary, uint32_t view_mask,
                                     std::span<const uint32_t> color_formats, uint32_t depth_format, uint32_t stencil_format,
                                     uint32_t rasterization_samples, uint32_t rendering_flags);
        int32_t cmd_execute_commands(uint64_t command_buffer, std::span<const uint64_t> secondaries);
        int32_t end_command_buffer(uint64_t command_buffer);
        int32_t reset_command_pool(uint64_t device, uint64_t pool, uint32_t flags);
        int32_t reset_command_buffer(uint64_t command_buffer, uint32_t flags);

        int32_t create_fence(uint64_t device, uint32_t flags, uint64_t& out_fence);
        void destroy_fence(uint64_t device, uint64_t fence);
        int32_t reset_fence(uint64_t device, uint64_t fence);
        int32_t create_event(uint64_t device, uint32_t flags, uint64_t& out_event);
        void destroy_event(uint64_t device, uint64_t event);
        int32_t get_event_status(uint64_t event);
        int32_t set_event(uint64_t device, uint64_t event);
        int32_t reset_event(uint64_t device, uint64_t event);

        int32_t create_semaphore(uint64_t device, uint32_t flags, uint32_t semaphore_type, uint64_t initial_value, uint64_t& out_semaphore);
        void destroy_semaphore(uint64_t device, uint64_t semaphore);
        int32_t get_semaphore_counter_value(uint64_t device, uint64_t semaphore, uint64_t& out_value);
        int32_t signal_semaphore(uint64_t device, uint64_t semaphore, uint64_t value);
        int32_t wait_semaphores(uint64_t device, uint32_t flags, const void* entries, uint32_t count, uint64_t timeout);
        uint64_t get_buffer_device_address(uint64_t device, uint64_t buffer);

        // Non-blocking: returns VK_SUCCESS if signaled, VK_NOT_READY otherwise. Never waits.
        int32_t get_fence_status(uint64_t fence);

        int32_t queue_wait_idle(uint64_t queue);
        int32_t device_wait_idle(uint64_t device);

        // Submits a single command buffer to the queue, optionally signaling the fence (0 = none).
        int32_t queue_submit(uint64_t queue, uint64_t command_buffer, uint64_t fence);

        // synchronization2 submit. wait_entries/signal_entries are submit2_semaphore_entry arrays;
        // command_buffer_ids is an object_id array. fence 0 = none.
        int32_t queue_submit2(uint64_t queue, uint64_t fence, const void* wait_entries, uint32_t wait_count, const void* command_buffer_ids,
                              uint32_t command_buffer_count, const void* signal_entries, uint32_t signal_count);

        // Writes up to out_size bytes of the device's VkPhysicalDeviceMemoryProperties into out.
        int32_t get_physical_device_memory_properties(uint64_t physical_device, void* out, size_t out_size);

        // Allocates a single VkDeviceMemory of `size` from `memory_type_index`. out_memory receives a
        // fresh object id, or 0 on failure.
        int32_t allocate_memory(uint64_t device, uint64_t size, uint32_t memory_type_index, uint64_t& out_memory);
        void free_memory(uint64_t device, uint64_t memory);

        int32_t create_buffer(uint64_t device, uint64_t size, uint32_t usage, uint64_t& out_buffer);
        void destroy_buffer(uint64_t device, uint64_t buffer);

        int32_t get_buffer_memory_requirements(uint64_t device, uint64_t buffer, uint64_t& out_size, uint64_t& out_alignment,
                                               uint32_t& out_memory_type_bits);
        int32_t bind_buffer_memory(uint64_t device, uint64_t buffer, uint64_t memory, uint64_t offset);

        // Records vkCmdFillBuffer into the (recording) command buffer.
        int32_t cmd_fill_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint64_t size, uint32_t data);

        // Maps host-visible memory, copies [offset, offset+size) into out, and unmaps. Used to read
        // GPU results back to the guest (the guest never sees a host pointer).
        int32_t download_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, void* out, size_t out_size);
        // Maps host-visible memory and copies `data` into [offset, offset+size). Persists guest writes.
        int32_t upload_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, const void* data, size_t data_size);
        int32_t flush_mapped_memory_range(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size);
        int32_t invalidate_mapped_memory_range(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size);
        // Maps host-visible memory and returns the persistent host pointer (for aliasing into the guest).
        int32_t map_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, void*& out_host_pointer);
        // Unmaps a mapping previously returned by map_memory.
        void unmap_memory(uint64_t device, uint64_t memory);

        // A VkImageSubresourceRange as plain integers (this header stays free of Vulkan types).
        struct subresource_range
        {
            uint32_t aspect_mask;
            uint32_t base_mip_level;
            uint32_t level_count;
            uint32_t base_array_layer;
            uint32_t layer_count;
        };

        // One region of a buffer-to-buffer copy (a VkBufferCopy as plain integers).
        struct buffer_copy
        {
            uint64_t src_offset;
            uint64_t dst_offset;
            uint64_t size;
        };

        // One VkRenderingAttachmentInfo (dynamic rendering) as plain integers. image_view is an object id
        // (0 = unused). clear_value holds the 16-byte VkClearValue union.
        struct rendering_attachment
        {
            uint64_t image_view;
            uint64_t resolve_image_view;
            uint32_t image_layout;
            uint32_t resolve_image_layout;
            uint32_t resolve_mode;
            uint32_t load_op;
            uint32_t store_op;
            std::array<uint32_t, 4> clear_value{};
        };

        // One VkViewport as plain floats (dynamic viewport state).
        struct viewport_entry
        {
            float x;
            float y;
            float width;
            float height;
            float min_depth;
            float max_depth;
        };

        // One VkRect2D as plain integers (dynamic scissor state).
        struct scissor_entry
        {
            int32_t offset_x;
            int32_t offset_y;
            uint32_t width;
            uint32_t height;
        };

        // Creates a 2D, single-mip, single-layer image (initial layout UNDEFINED). samples selects the
        // multisample count (1 = no MSAA).
        int32_t create_image(uint64_t device, uint32_t format, uint32_t width, uint32_t height, uint32_t usage, uint32_t tiling,
                             uint32_t samples, uint32_t image_type, uint32_t depth, uint32_t mip_levels, uint32_t array_layers,
                             uint32_t flags, uint64_t& out_image);
        void destroy_image(uint64_t device, uint64_t image);
        int32_t get_image_subresource_layout(uint64_t device, uint64_t image, uint32_t aspect_mask, uint32_t mip_level,
                                             uint32_t array_layer, uint64_t& out_offset, uint64_t& out_size, uint64_t& out_row_pitch,
                                             uint64_t& out_array_pitch, uint64_t& out_depth_pitch);
        int32_t get_image_memory_requirements(uint64_t device, uint64_t image, uint64_t& out_size, uint64_t& out_alignment,
                                              uint32_t& out_memory_type_bits);
        int32_t bind_image_memory(uint64_t device, uint64_t image, uint64_t memory, uint64_t offset);

        // Records a single image memory barrier into the (recording) command buffer.
        int32_t cmd_pipeline_barrier(uint64_t command_buffer, uint64_t image, uint32_t src_stage_mask, uint32_t dst_stage_mask,
                                     uint32_t src_access_mask, uint32_t dst_access_mask, uint32_t old_layout, uint32_t new_layout,
                                     const subresource_range& range);
        // Records vkCmdClearColorImage with an RGBA float clear color.
        int32_t cmd_clear_color_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float r, float g, float b, float a,
                                      const subresource_range& range);
        int32_t cmd_clear_depth_stencil_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float depth, uint32_t stencil,
                                              const subresource_range& range);
        // Copies mip 0 / layer 0 of the image (tightly packed) into the buffer at offset 0.
        int32_t cmd_copy_image_to_buffer(uint64_t command_buffer, uint64_t image, uint32_t image_layout, uint64_t buffer, uint32_t width,
                                         uint32_t height, uint32_t aspect_mask);
        // Copies tightly-packed pixel data from the buffer (offset 0) into mip 0 / layer 0 of the image.
        // One VkBufferImageCopy region. DXVK sub-allocates uploads from a shared staging buffer, so
        // buffer_offset (and mip/layer/image_offset) are routinely non-zero and must be honoured.
        struct buffer_image_copy_region
        {
            uint64_t buffer_offset;
            uint32_t buffer_row_length;
            uint32_t buffer_image_height;
            int32_t image_offset_x;
            int32_t image_offset_y;
            int32_t image_offset_z;
            uint32_t width;
            uint32_t height;
            uint32_t depth;
            uint32_t mip_level;
            uint32_t base_array_layer;
            uint32_t layer_count;
            uint32_t aspect_mask;
        };

        int32_t cmd_copy_buffer_to_image(uint64_t command_buffer, uint64_t buffer, uint64_t image, uint32_t image_layout,
                                         const buffer_image_copy_region& region);
        // One VkImageCopy region of an image-to-image copy (vkCmdCopyImage[2]).
        struct image_copy_region
        {
            uint32_t src_aspect_mask;
            uint32_t src_mip_level;
            uint32_t src_base_array_layer;
            uint32_t src_layer_count;
            int32_t src_offset_x;
            int32_t src_offset_y;
            int32_t src_offset_z;
            uint32_t dst_aspect_mask;
            uint32_t dst_mip_level;
            uint32_t dst_base_array_layer;
            uint32_t dst_layer_count;
            int32_t dst_offset_x;
            int32_t dst_offset_y;
            int32_t dst_offset_z;
            uint32_t width;
            uint32_t height;
            uint32_t depth;
        };
        int32_t cmd_copy_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image, uint32_t dst_layout,
                               const image_copy_region& region);
        // Resolves a multisampled source image into a single-sample destination (full image, mip 0 / layer 0).
        int32_t cmd_resolve_image(uint64_t command_buffer, uint64_t src_image, uint32_t src_layout, uint64_t dst_image, uint32_t dst_layout,
                                  uint32_t width, uint32_t height, uint32_t aspect_mask);
        // Updates a buffer region inline from caller-provided data (vkCmdUpdateBuffer).
        int32_t cmd_update_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, const void* data, uint32_t size);

        // A linear/nearest sampler (mag/min filter + per-axis address modes; no mipmapping/anisotropy).
        int32_t create_sampler(uint64_t device, uint32_t mag_filter, uint32_t min_filter, uint32_t address_mode_u, uint32_t address_mode_v,
                               uint32_t address_mode_w, uint64_t& out_sampler);
        void destroy_sampler(uint64_t device, uint64_t sampler);

        // --- WSI (modeled with offscreen images; "present" reads back and hands pixels to the UI) ---

        // A surface is just the guest HWND to present to; no real host VkSurfaceKHR is created.
        int32_t create_surface(uint64_t hwnd, uint64_t& out_surface);
        void destroy_surface(uint64_t surface);

        // Writes synthetic VkSurfaceCapabilitiesKHR into out; currentExtent is left undefined
        // (0xFFFFFFFF) so the guest chooses the swapchain extent.
        uint64_t get_surface_hwnd(uint64_t surface) const;
        int32_t get_surface_capabilities(uint64_t physical_device, uint64_t surface, uint32_t window_width, uint32_t window_height,
                                         void* out, size_t out_size);

        // Creates `min_image_count` (>= 2) offscreen images of the given format/extent plus a readback
        // buffer. out_image_count receives the number of images created.
        int32_t create_swapchain(uint64_t device, uint64_t surface, uint32_t format, uint32_t width, uint32_t height,
                                 uint32_t min_image_count, uint32_t image_usage, uint64_t& out_swapchain, uint32_t& out_image_count);
        void destroy_swapchain(uint64_t device, uint64_t swapchain);

        // Writes the swapchain's image object ids into out_images; out_count gets the true count.
        int32_t get_swapchain_images(uint64_t swapchain, std::span<uint64_t> out_images, uint32_t& out_count);

        // Returns the next image index (round-robin; images are always immediately available). Since the
        // image is ready immediately, the caller's acquire semaphore/fence are signalled right away so the
        // render submit that waits on them can proceed (0 = none).
        int32_t acquire_next_image(uint64_t swapchain, uint64_t semaphore, uint64_t fence, uint32_t& out_index);

        // Copies the presented image into the readback buffer (blocking on the queue), then fills
        // out_pixels (BGRA, width*height*4) and reports the target window/extent so the caller can hand
        // the pixels to the UI backend.
        int32_t queue_present(uint64_t queue, uint64_t swapchain, uint32_t image_index, std::vector<std::byte>& out_pixels,
                              uint32_t& out_width, uint32_t& out_height, uint64_t& out_hwnd);

        // --- graphics pipeline (enough for a render-pass triangle) ---

        int32_t create_shader_module(uint64_t device, const void* code, size_t code_size, uint64_t& out_module);
        void destroy_shader_module(uint64_t device, uint64_t shader_module);

        // aspect_mask selects COLOR vs DEPTH (0 defaults to COLOR).
        int32_t create_image_view(uint64_t device, uint64_t image, uint32_t format, uint32_t aspect_mask, uint32_t view_type,
                                  uint32_t base_mip_level, uint32_t level_count, uint32_t base_array_layer, uint32_t layer_count,
                                  uint32_t swizzle_r, uint32_t swizzle_g, uint32_t swizzle_b, uint32_t swizzle_a, uint64_t& out_view);
        void destroy_image_view(uint64_t device, uint64_t image_view);
        int32_t create_buffer_view(uint64_t device, uint64_t buffer, uint32_t format, uint64_t offset, uint64_t range, uint64_t& out_view);
        void destroy_buffer_view(uint64_t device, uint64_t buffer_view);
        int32_t cmd_copy_buffer(uint64_t command_buffer, uint64_t src_buffer, uint64_t dst_buffer, std::span<const buffer_copy> regions);
        int32_t create_query_pool(uint64_t device, uint32_t query_type, uint32_t query_count, uint32_t pipeline_statistics,
                                  uint64_t& out_pool);
        void destroy_query_pool(uint64_t device, uint64_t query_pool);
        int32_t reset_query_pool(uint64_t device, uint64_t query_pool, uint32_t first_query, uint32_t query_count);
        int32_t get_query_pool_results(uint64_t device, uint64_t query_pool, uint32_t first_query, uint32_t query_count, uint32_t flags,
                                       void* out, size_t out_size, size_t stride, size_t& out_written);
        int32_t cmd_reset_query_pool(uint64_t command_buffer, uint64_t query_pool, uint32_t first_query, uint32_t query_count);
        int32_t cmd_begin_query(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t flags);
        int32_t cmd_end_query(uint64_t command_buffer, uint64_t query_pool, uint32_t query);
        int32_t cmd_write_timestamp(uint64_t command_buffer, uint64_t query_pool, uint32_t query, uint32_t pipeline_stage);

        // One color attachment + an optional depth attachment (depth_format == 0 => color only), single
        // subpass (initial/final layouts as given; PRESENT_SRC_KHR is mapped to TRANSFER_SRC_OPTIMAL).
        int32_t create_render_pass(uint64_t device, uint32_t format, uint32_t load_op, uint32_t store_op, uint32_t initial_layout,
                                   uint32_t final_layout, uint32_t depth_format, uint64_t& out_render_pass);
        void destroy_render_pass(uint64_t device, uint64_t render_pass);

        // depth_view == 0 => single color attachment.
        int32_t create_framebuffer(uint64_t device, uint64_t render_pass, uint64_t image_view, uint64_t depth_view, uint32_t width,
                                   uint32_t height, uint64_t& out_framebuffer);
        void destroy_framebuffer(uint64_t device, uint64_t framebuffer);

        // --- descriptor sets (uniform buffers now; combined image samplers added with textures) ---

        // VkDescriptorSetLayoutBinding as plain integers.
        struct descriptor_binding
        {
            uint32_t binding;
            uint32_t descriptor_type;
            uint32_t descriptor_count;
            uint32_t stage_flags;
        };
        // VkDescriptorPoolSize as plain integers.
        struct descriptor_pool_size
        {
            uint32_t descriptor_type;
            uint32_t descriptor_count;
        };
        // A single descriptor write (one descriptor). For buffer types buffer/offset/range apply; for
        // image types (combined image sampler) sampler/image_view/image_layout apply.
        struct descriptor_write
        {
            uint64_t dst_set;
            uint32_t dst_binding;
            uint32_t dst_array_element;
            uint32_t descriptor_type;
            uint64_t buffer;
            uint64_t offset;
            uint64_t range;
            uint64_t sampler;
            uint64_t image_view;
            uint32_t image_layout;
        };

        int32_t create_descriptor_set_layout(uint64_t device, std::span<const descriptor_binding> bindings, uint64_t& out_layout);
        void destroy_descriptor_set_layout(uint64_t device, uint64_t layout);
        int32_t create_descriptor_pool(uint64_t device, uint32_t max_sets, std::span<const descriptor_pool_size> sizes, uint64_t& out_pool);
        void destroy_descriptor_pool(uint64_t device, uint64_t pool);
        // Allocates one set per layout id; writes the allocated set ids into out_sets, out_count gets the
        // true count.
        int32_t allocate_descriptor_sets(uint64_t device, uint64_t pool, std::span<const uint64_t> set_layouts,
                                         std::span<uint64_t> out_sets, uint32_t& out_count);
        int32_t update_descriptor_sets(uint64_t device, std::span<const descriptor_write> writes);

        // Optionally one push-constant range from offset 0 (push_constant_size == 0 means none), plus a
        // list of descriptor-set layouts (empty for none).
        int32_t create_pipeline_layout(uint64_t device, uint32_t push_constant_stages, uint32_t push_constant_size,
                                       std::span<const uint64_t> set_layouts, uint64_t& out_layout);
        void destroy_pipeline_layout(uint64_t device, uint64_t pipeline_layout);

        // VkVertexInputBindingDescription / VkVertexInputAttributeDescription as plain integers (this
        // header stays free of Vulkan types).
        struct vertex_binding
        {
            uint32_t binding;
            uint32_t stride;
            uint32_t input_rate;
        };
        struct vertex_attribute
        {
            uint32_t location;
            uint32_t binding;
            uint32_t format;
            uint32_t offset;
        };

        // Optional depth-stencil state for a pipeline (test_enable == 0 => none, as before).
        struct depth_state
        {
            uint32_t test_enable;
            uint32_t write_enable;
            uint32_t compare_op;
        };

        struct spec_entry
        {
            uint32_t constant_id;
            uint32_t offset;
            uint32_t size;
        };

        // Per-color-attachment blend state. DXVK bakes D3D9 alpha blending statically; without honoring it
        // transparent geometry renders fully opaque.
        struct color_blend_attachment
        {
            uint32_t blend_enable;
            uint32_t src_color_blend_factor;
            uint32_t dst_color_blend_factor;
            uint32_t color_blend_op;
            uint32_t src_alpha_blend_factor;
            uint32_t dst_alpha_blend_factor;
            uint32_t alpha_blend_op;
            uint32_t color_write_mask;
        };

        // A shader stage's specialization constants. DXVK bakes d3d9 render state (alpha-test compare op, fog,
        // sampler types, ...) into shaders this way; without it every constant defaults to 0, turning the
        // alpha-test op into VK_COMPARE_OP_NEVER and discarding every fragment.
        struct specialization
        {
            std::span<const spec_entry> entries;
            std::span<const uint8_t> data;
        };

        // Triangle list, static full-extent viewport/scissor, one non-blended color attachment, optional
        // depth test. Empty vertex input (no bindings/attributes) leaves vertices to be baked into the shader.
        // When render_pass == 0 the pipeline is built for dynamic rendering (VK_KHR_dynamic_rendering) using
        // color_formats/depth_format/stencil_format, with viewport and scissor as dynamic state.
        int32_t create_graphics_pipeline(uint64_t device, uint64_t render_pass, uint64_t pipeline_layout, uint64_t vertex_shader,
                                         uint64_t fragment_shader, uint32_t width, uint32_t height,
                                         std::span<const vertex_binding> bindings, std::span<const vertex_attribute> attributes,
                                         const depth_state& depth, std::span<const uint32_t> color_formats, uint32_t depth_format,
                                         uint32_t stencil_format, uint32_t rasterization_samples, std::span<const uint32_t> dynamic_states,
                                         const specialization& vs_spec, const specialization& fs_spec,
                                         std::span<const color_blend_attachment> blend_attachments, uint64_t& out_pipeline);
        int32_t create_compute_pipeline(uint64_t device, uint64_t pipeline_layout, uint64_t shader_module, uint64_t& out_pipeline);
        void destroy_pipeline(uint64_t device, uint64_t pipeline);

        // clear_depth is used only when the render pass has a depth attachment.
        int32_t cmd_begin_render_pass(uint64_t command_buffer, uint64_t render_pass, uint64_t framebuffer, uint32_t width, uint32_t height,
                                      float r, float g, float b, float a, float clear_depth);
        int32_t cmd_bind_pipeline(uint64_t command_buffer, uint64_t pipeline, uint32_t bind_point);
        int32_t cmd_dispatch(uint64_t command_buffer, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);
        int32_t cmd_dispatch_indirect(uint64_t command_buffer, uint64_t buffer, uint64_t offset);
        int32_t cmd_draw(uint64_t command_buffer, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                         uint32_t first_instance);
        // Binds `count` vertex buffers (parallel buffer-id / offset arrays) starting at first_binding.
        int32_t cmd_bind_vertex_buffers(uint64_t command_buffer, uint32_t first_binding, uint32_t count, const uint64_t* buffer_ids,
                                        const uint64_t* offsets);
        int32_t cmd_bind_vertex_buffers2(uint64_t command_buffer, uint32_t first_binding, uint32_t count, const uint64_t* buffer_ids,
                                         const uint64_t* offsets, const uint64_t* sizes, const uint64_t* strides);
        int32_t cmd_bind_index_buffer(uint64_t command_buffer, uint64_t buffer, uint64_t offset, uint32_t index_type);
        int32_t cmd_draw_indexed(uint64_t command_buffer, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                 int32_t vertex_offset, uint32_t first_instance);
        int32_t cmd_bind_descriptor_sets(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t first_set,
                                         std::span<const uint64_t> sets, uint32_t bind_point, std::span<const uint32_t> dynamic_offsets);
        int32_t cmd_end_render_pass(uint64_t command_buffer);
        // Dynamic rendering (VK_KHR_dynamic_rendering / core 1.3). depth/stencil are null when absent.
        int32_t cmd_begin_rendering(uint64_t command_buffer, int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h,
                                    uint32_t layer_count, uint32_t view_mask, uint32_t flags, std::span<const rendering_attachment> color,
                                    const rendering_attachment* depth, const rendering_attachment* stencil);
        int32_t cmd_end_rendering(uint64_t command_buffer);
        int32_t cmd_push_constants(uint64_t command_buffer, uint64_t pipeline_layout, uint32_t stage_flags, uint32_t offset, uint32_t size,
                                   const void* data);
        // Extended-dynamic-state setters. with_count selects the *WithCount variant (dynamic count state).
        int32_t cmd_set_viewport(uint64_t command_buffer, uint32_t first, bool with_count, std::span<const viewport_entry> viewports);
        int32_t cmd_set_scissor(uint64_t command_buffer, uint32_t first, bool with_count, std::span<const scissor_entry> scissors);
        int32_t cmd_set_depth_bias(uint64_t command_buffer, float constant_factor, float clamp, float slope_factor);
        int32_t cmd_set_blend_constants(uint64_t command_buffer, const std::array<float, 4>& constants);
        int32_t cmd_set_depth_bounds(uint64_t command_buffer, float min_depth_bounds, float max_depth_bounds);
        int32_t cmd_set_line_width(uint64_t command_buffer, float line_width);
        int32_t cmd_set_stencil(uint64_t command_buffer, uint32_t which, uint32_t face_mask, uint32_t value);
        int32_t cmd_set_stencil_op(uint64_t command_buffer, uint32_t face_mask, uint32_t fail_op, uint32_t pass_op, uint32_t depth_fail_op,
                                   uint32_t compare_op);
        int32_t cmd_set_dynamic_u32(uint64_t command_buffer, uint32_t state, uint32_t value);

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
