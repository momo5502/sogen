#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

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

        // Writes up to out_size bytes of the device's VkPhysicalDeviceProperties into out.
        int32_t get_physical_device_properties(uint64_t physical_device, void* out, size_t out_size);

        // Writes the device's queue families as raw VkQueueFamilyProperties into out (sized in
        // bytes). out_count always receives the true family count.
        int32_t get_queue_family_properties(uint64_t physical_device, void* out, size_t out_size, uint32_t& out_count);

        // Creates a logical device with a single queue family (queue_count queues, default priority)
        // and no extensions/features. out_device receives a fresh object id, or 0 on failure.
        int32_t create_device(uint64_t physical_device, uint32_t queue_family_index, uint32_t queue_count, uint64_t& out_device);
        void destroy_device(uint64_t device);

        // Resolves a queue created with the device; out_queue receives a stable object id.
        int32_t get_device_queue(uint64_t device, uint32_t queue_family_index, uint32_t queue_index, uint64_t& out_queue);

        int32_t create_command_pool(uint64_t device, uint32_t queue_family_index, uint32_t flags, uint64_t& out_pool);
        void destroy_command_pool(uint64_t device, uint64_t pool);

        // Allocates a single primary command buffer from the pool.
        int32_t allocate_command_buffer(uint64_t device, uint64_t pool, uint64_t& out_command_buffer);
        void free_command_buffer(uint64_t device, uint64_t pool, uint64_t command_buffer);

        int32_t begin_command_buffer(uint64_t command_buffer, uint32_t flags);
        int32_t end_command_buffer(uint64_t command_buffer);

        int32_t create_fence(uint64_t device, uint32_t flags, uint64_t& out_fence);
        void destroy_fence(uint64_t device, uint64_t fence);
        int32_t reset_fence(uint64_t device, uint64_t fence);

        // Non-blocking: returns VK_SUCCESS if signaled, VK_NOT_READY otherwise. Never waits.
        int32_t get_fence_status(uint64_t fence);

        // Submits a single command buffer to the queue, optionally signaling the fence (0 = none).
        int32_t queue_submit(uint64_t queue, uint64_t command_buffer, uint64_t fence);

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
        int32_t upload_memory(uint64_t device, uint64_t memory, uint64_t offset, uint64_t size, const void* data,
                              size_t data_size);

        // A VkImageSubresourceRange as plain integers (this header stays free of Vulkan types).
        struct subresource_range
        {
            uint32_t aspect_mask;
            uint32_t base_mip_level;
            uint32_t level_count;
            uint32_t base_array_layer;
            uint32_t layer_count;
        };

        // Creates a 2D, single-mip, single-layer image (samples = 1, initial layout UNDEFINED).
        int32_t create_image(uint64_t device, uint32_t format, uint32_t width, uint32_t height, uint32_t usage,
                             uint32_t tiling, uint64_t& out_image);
        void destroy_image(uint64_t device, uint64_t image);
        int32_t get_image_memory_requirements(uint64_t device, uint64_t image, uint64_t& out_size, uint64_t& out_alignment,
                                              uint32_t& out_memory_type_bits);
        int32_t bind_image_memory(uint64_t device, uint64_t image, uint64_t memory, uint64_t offset);

        // Records a single image memory barrier into the (recording) command buffer.
        int32_t cmd_pipeline_barrier(uint64_t command_buffer, uint64_t image, uint32_t src_stage_mask,
                                     uint32_t dst_stage_mask, uint32_t src_access_mask, uint32_t dst_access_mask,
                                     uint32_t old_layout, uint32_t new_layout, const subresource_range& range);
        // Records vkCmdClearColorImage with an RGBA float clear color.
        int32_t cmd_clear_color_image(uint64_t command_buffer, uint64_t image, uint32_t image_layout, float r, float g,
                                      float b, float a, const subresource_range& range);
        // Copies mip 0 / layer 0 of the image (tightly packed) into the buffer at offset 0.
        int32_t cmd_copy_image_to_buffer(uint64_t command_buffer, uint64_t image, uint32_t image_layout, uint64_t buffer,
                                         uint32_t width, uint32_t height, uint32_t aspect_mask);

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
