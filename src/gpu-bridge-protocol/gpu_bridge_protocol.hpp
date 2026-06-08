#pragma once

// Wire protocol for the Sogen GPU paravirtualization bridge.
//
// The guest talks to the host through a virtual driver device (\\.\SogenGpu, i.e.
// \Device\SogenGpu) using NtDeviceIoControlFile. The IOCTL code selects a command;
// the input/output buffers carry the (de)serialized payload. This header is the single
// source of truth for that boundary and is intentionally dependency-free (only <cstdint>)
// so it can be included unchanged by both the emulator and guest-side shims.

#include <cstdint>

namespace sogen::gpu_bridge
{
    // Identifies a valid bridge and lets the guest detect a host that speaks a different
    // protocol revision before issuing any further commands.
    inline constexpr uint32_t protocol_magic = 0x55504753;   // 'SGPU'
    inline constexpr uint32_t protocol_version = 1;

    // Windows IOCTL encoding: CTL_CODE(DeviceType, Function, Method, Access).
    //   value = (DeviceType << 16) | (Access << 14) | (Function << 2) | Method
    // We use FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED (0) and FILE_ANY_ACCESS (0), and
    // keep functions in the vendor-defined range (>= 0x800), matching how a real driver
    // would lay out its private control codes.
    inline constexpr uint32_t device_type = 0x22;            // FILE_DEVICE_UNKNOWN
    inline constexpr uint32_t method_buffered = 0;
    inline constexpr uint32_t file_any_access = 0;

    constexpr uint32_t make_ioctl(const uint32_t function)
    {
        return (device_type << 16) | (file_any_access << 14) | (function << 2) | method_buffered;
    }

    enum class command : uint32_t
    {
        get_version = 0x800,
        create_instance = 0x801,
        destroy_instance = 0x802,
        enumerate_physical_devices = 0x803,
        get_physical_device_properties = 0x804,
        get_queue_family_properties = 0x805,
        create_device = 0x806,
        get_device_queue = 0x807,
        destroy_device = 0x808,
        create_command_pool = 0x809,
        destroy_command_pool = 0x80A,
        allocate_command_buffer = 0x80B,
        free_command_buffer = 0x80C,
        begin_command_buffer = 0x80D,
        end_command_buffer = 0x80E,
        create_fence = 0x80F,
        destroy_fence = 0x810,
        reset_fence = 0x811,
        get_fence_status = 0x812,
        queue_submit = 0x813,
        get_physical_device_memory_properties = 0x814,
        allocate_memory = 0x815,
        free_memory = 0x816,
        create_buffer = 0x817,
        destroy_buffer = 0x818,
        get_buffer_memory_requirements = 0x819,
        bind_buffer_memory = 0x81A,
        cmd_fill_buffer = 0x81B,
        download_memory = 0x81C,
        upload_memory = 0x81D,
        create_image = 0x81E,
        destroy_image = 0x81F,
        get_image_memory_requirements = 0x820,
        bind_image_memory = 0x821,
        cmd_pipeline_barrier = 0x822,
        cmd_clear_color_image = 0x823,
        cmd_copy_image_to_buffer = 0x824,
    };

    inline constexpr uint32_t ioctl_get_version = make_ioctl(static_cast<uint32_t>(command::get_version));
    inline constexpr uint32_t ioctl_create_instance = make_ioctl(static_cast<uint32_t>(command::create_instance));
    inline constexpr uint32_t ioctl_destroy_instance = make_ioctl(static_cast<uint32_t>(command::destroy_instance));
    inline constexpr uint32_t ioctl_enumerate_physical_devices =
        make_ioctl(static_cast<uint32_t>(command::enumerate_physical_devices));
    inline constexpr uint32_t ioctl_get_physical_device_properties =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_properties));
    inline constexpr uint32_t ioctl_get_queue_family_properties =
        make_ioctl(static_cast<uint32_t>(command::get_queue_family_properties));
    inline constexpr uint32_t ioctl_create_device = make_ioctl(static_cast<uint32_t>(command::create_device));
    inline constexpr uint32_t ioctl_get_device_queue = make_ioctl(static_cast<uint32_t>(command::get_device_queue));
    inline constexpr uint32_t ioctl_destroy_device = make_ioctl(static_cast<uint32_t>(command::destroy_device));
    inline constexpr uint32_t ioctl_create_command_pool = make_ioctl(static_cast<uint32_t>(command::create_command_pool));
    inline constexpr uint32_t ioctl_destroy_command_pool = make_ioctl(static_cast<uint32_t>(command::destroy_command_pool));
    inline constexpr uint32_t ioctl_allocate_command_buffer = make_ioctl(static_cast<uint32_t>(command::allocate_command_buffer));
    inline constexpr uint32_t ioctl_free_command_buffer = make_ioctl(static_cast<uint32_t>(command::free_command_buffer));
    inline constexpr uint32_t ioctl_begin_command_buffer = make_ioctl(static_cast<uint32_t>(command::begin_command_buffer));
    inline constexpr uint32_t ioctl_end_command_buffer = make_ioctl(static_cast<uint32_t>(command::end_command_buffer));
    inline constexpr uint32_t ioctl_create_fence = make_ioctl(static_cast<uint32_t>(command::create_fence));
    inline constexpr uint32_t ioctl_destroy_fence = make_ioctl(static_cast<uint32_t>(command::destroy_fence));
    inline constexpr uint32_t ioctl_reset_fence = make_ioctl(static_cast<uint32_t>(command::reset_fence));
    inline constexpr uint32_t ioctl_get_fence_status = make_ioctl(static_cast<uint32_t>(command::get_fence_status));
    inline constexpr uint32_t ioctl_queue_submit = make_ioctl(static_cast<uint32_t>(command::queue_submit));
    inline constexpr uint32_t ioctl_get_physical_device_memory_properties =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_memory_properties));
    inline constexpr uint32_t ioctl_allocate_memory = make_ioctl(static_cast<uint32_t>(command::allocate_memory));
    inline constexpr uint32_t ioctl_free_memory = make_ioctl(static_cast<uint32_t>(command::free_memory));
    inline constexpr uint32_t ioctl_create_buffer = make_ioctl(static_cast<uint32_t>(command::create_buffer));
    inline constexpr uint32_t ioctl_destroy_buffer = make_ioctl(static_cast<uint32_t>(command::destroy_buffer));
    inline constexpr uint32_t ioctl_get_buffer_memory_requirements =
        make_ioctl(static_cast<uint32_t>(command::get_buffer_memory_requirements));
    inline constexpr uint32_t ioctl_bind_buffer_memory = make_ioctl(static_cast<uint32_t>(command::bind_buffer_memory));
    inline constexpr uint32_t ioctl_cmd_fill_buffer = make_ioctl(static_cast<uint32_t>(command::cmd_fill_buffer));
    inline constexpr uint32_t ioctl_download_memory = make_ioctl(static_cast<uint32_t>(command::download_memory));
    inline constexpr uint32_t ioctl_upload_memory = make_ioctl(static_cast<uint32_t>(command::upload_memory));
    inline constexpr uint32_t ioctl_create_image = make_ioctl(static_cast<uint32_t>(command::create_image));
    inline constexpr uint32_t ioctl_destroy_image = make_ioctl(static_cast<uint32_t>(command::destroy_image));
    inline constexpr uint32_t ioctl_get_image_memory_requirements =
        make_ioctl(static_cast<uint32_t>(command::get_image_memory_requirements));
    inline constexpr uint32_t ioctl_bind_image_memory = make_ioctl(static_cast<uint32_t>(command::bind_image_memory));
    inline constexpr uint32_t ioctl_cmd_pipeline_barrier = make_ioctl(static_cast<uint32_t>(command::cmd_pipeline_barrier));
    inline constexpr uint32_t ioctl_cmd_clear_color_image =
        make_ioctl(static_cast<uint32_t>(command::cmd_clear_color_image));
    inline constexpr uint32_t ioctl_cmd_copy_image_to_buffer =
        make_ioctl(static_cast<uint32_t>(command::cmd_copy_image_to_buffer));

    // Opaque identifier handed to the guest in place of a host Vulkan handle. The host keeps the
    // real VkInstance / VkPhysicalDevice / ... in a table and the guest only ever sees this id, so
    // raw host pointers never cross the boundary.
    using object_id = uint64_t;

    inline constexpr object_id null_object = 0;

    // Real Vulkan structs (VkPhysicalDeviceProperties, ...) ride the wire as raw bytes; both sides
    // agree on their layout through their own <vulkan/vulkan_core.h>. This keeps the protocol header
    // dependency-free while still passing native structures unchanged.

    // ioctl_get_version: out
    struct version_response
    {
        uint32_t magic;   // protocol_magic
        uint32_t version; // protocol_version
    };

    // ioctl_create_instance: out (no input for now; a bare instance with no layers/extensions)
    struct create_instance_response
    {
        int32_t vk_result;  // VkResult from the host
        uint32_t reserved;  // padding / future flags
        object_id instance; // null_object on failure
    };

    // ioctl_destroy_instance: in
    struct destroy_instance_request
    {
        object_id instance;
    };

    // ioctl_enumerate_physical_devices: in
    struct enumerate_physical_devices_request
    {
        object_id instance;
        uint32_t max_count; // capacity of the device-id array in the output buffer (after the header)
        uint32_t reserved;
    };

    // ioctl_enumerate_physical_devices: out header, immediately followed by `count` object_id entries
    struct enumerate_physical_devices_response
    {
        int32_t vk_result;
        uint32_t count; // number of physical devices reported by the host
        // object_id devices[count];
    };

    // ioctl_get_physical_device_properties: in (out = raw VkPhysicalDeviceProperties bytes)
    struct get_physical_device_properties_request
    {
        object_id physical_device;
    };

    // ioctl_get_queue_family_properties: in
    struct get_queue_family_properties_request
    {
        object_id physical_device;
        uint32_t max_count; // capacity (in entries) of the array following the response header
        uint32_t reserved;
    };

    // ioctl_get_queue_family_properties: out header, followed by `count` raw VkQueueFamilyProperties
    struct get_queue_family_properties_response
    {
        uint32_t count; // true number of queue families on the device
        uint32_t reserved;
    };

    // ioctl_create_device: in (single queue from one family; no extensions/features for now)
    struct create_device_request
    {
        object_id physical_device;
        uint32_t queue_family_index;
        uint32_t queue_count;
    };

    // ioctl_create_device: out
    struct create_device_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id device; // null_object on failure
    };

    // ioctl_get_device_queue: in
    struct get_device_queue_request
    {
        object_id device;
        uint32_t queue_family_index;
        uint32_t queue_index;
    };

    // ioctl_get_device_queue: out
    struct get_device_queue_response
    {
        object_id queue; // null_object on failure
    };

    // ioctl_destroy_device: in
    struct destroy_device_request
    {
        object_id device;
    };

    // Generic output for commands that only report a VkResult.
    struct result_response
    {
        int32_t vk_result;
        uint32_t reserved;
    };

    // ioctl_create_command_pool: in (flags = VkCommandPoolCreateFlags)
    struct create_command_pool_request
    {
        object_id device;
        uint32_t queue_family_index;
        uint32_t flags;
    };

    // ioctl_create_command_pool: out
    struct create_command_pool_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id command_pool;
    };

    // ioctl_destroy_command_pool: in
    struct destroy_command_pool_request
    {
        object_id device;
        object_id command_pool;
    };

    // ioctl_allocate_command_buffer: in (a single primary command buffer)
    struct allocate_command_buffer_request
    {
        object_id device;
        object_id command_pool;
    };

    // ioctl_allocate_command_buffer: out
    struct allocate_command_buffer_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id command_buffer;
    };

    // ioctl_free_command_buffer: in
    struct free_command_buffer_request
    {
        object_id device;
        object_id command_pool;
        object_id command_buffer;
    };

    // ioctl_begin_command_buffer: in (flags = VkCommandBufferUsageFlags); out = result_response
    struct begin_command_buffer_request
    {
        object_id command_buffer;
        uint32_t flags;
        uint32_t reserved;
    };

    // ioctl_end_command_buffer: in; out = result_response
    struct end_command_buffer_request
    {
        object_id command_buffer;
    };

    // ioctl_create_fence: in (flags = VkFenceCreateFlags, e.g. VK_FENCE_CREATE_SIGNALED_BIT)
    struct create_fence_request
    {
        object_id device;
        uint32_t flags;
        uint32_t reserved;
    };

    // ioctl_create_fence: out
    struct create_fence_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id fence;
    };

    // ioctl_destroy_fence: in
    struct destroy_fence_request
    {
        object_id device;
        object_id fence;
    };

    // ioctl_reset_fence: in; out = result_response
    struct reset_fence_request
    {
        object_id device;
        object_id fence;
    };

    // ioctl_get_fence_status: in; out = result_response with vk_result = VK_SUCCESS (signaled) /
    // VK_NOT_READY (unsignaled). This is the non-blocking poll the guest spins on while yielding, so
    // the host thread is never blocked on the GPU.
    struct get_fence_status_request
    {
        object_id fence;
    };

    // ioctl_queue_submit: in (one command buffer, optional fence, no semaphores); out = result_response
    struct queue_submit_request
    {
        object_id queue;
        object_id command_buffer;
        object_id fence; // null_object for no fence
    };

    // ioctl_get_physical_device_memory_properties: in (out = raw VkPhysicalDeviceMemoryProperties bytes)
    struct get_physical_device_memory_properties_request
    {
        object_id physical_device;
    };

    // ioctl_allocate_memory: in (a single VkDeviceMemory allocation of `size` from `memory_type_index`)
    struct allocate_memory_request
    {
        object_id device;
        uint64_t size; // VkDeviceSize allocationSize
        uint32_t memory_type_index;
        uint32_t reserved;
    };

    // ioctl_allocate_memory: out
    struct allocate_memory_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id memory; // null_object on failure
    };

    // ioctl_free_memory: in
    struct free_memory_request
    {
        object_id device;
        object_id memory;
    };

    // ioctl_create_buffer: in (usage = VkBufferUsageFlags; exclusive sharing, no queue families)
    struct create_buffer_request
    {
        object_id device;
        uint64_t size; // VkDeviceSize
        uint32_t usage;
        uint32_t reserved;
    };

    // ioctl_create_buffer: out
    struct create_buffer_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id buffer; // null_object on failure
    };

    // ioctl_destroy_buffer: in
    struct destroy_buffer_request
    {
        object_id device;
        object_id buffer;
    };

    // ioctl_get_buffer_memory_requirements: in
    struct get_buffer_memory_requirements_request
    {
        object_id device;
        object_id buffer;
    };

    // VkMemoryRequirements carried as explicit fields (shared by buffers and, later, images).
    struct memory_requirements_response
    {
        int32_t vk_result;
        uint32_t memory_type_bits;
        uint64_t size;      // VkDeviceSize
        uint64_t alignment; // VkDeviceSize
    };

    // ioctl_bind_buffer_memory: in; out = result_response
    struct bind_buffer_memory_request
    {
        object_id device;
        object_id buffer;
        object_id memory;
        uint64_t offset; // VkDeviceSize
    };

    // ioctl_cmd_fill_buffer: in (records vkCmdFillBuffer into the command buffer); out = result_response
    struct cmd_fill_buffer_request
    {
        object_id command_buffer;
        object_id buffer;
        uint64_t offset; // VkDeviceSize
        uint64_t size;   // VkDeviceSize (VK_WHOLE_SIZE allowed)
        uint32_t data;   // 32-bit value broadcast across the range
        uint32_t reserved;
    };

    // ioctl_download_memory: in (out = `size` raw bytes read from host-mapped memory)
    struct download_memory_request
    {
        object_id device;
        object_id memory;
        uint64_t offset; // VkDeviceSize
        uint64_t size;   // VkDeviceSize
    };

    // ioctl_upload_memory: in header immediately followed by `size` raw bytes; out = result_response
    struct upload_memory_request
    {
        object_id device;
        object_id memory;
        uint64_t offset; // VkDeviceSize
        uint64_t size;   // VkDeviceSize
        // uint8_t bytes[size];
    };

    // A VkImageSubresourceRange flattened to plain integers (aspect/mips/layers).
    struct image_subresource_range
    {
        uint32_t aspect_mask; // VkImageAspectFlags
        uint32_t base_mip_level;
        uint32_t level_count;
        uint32_t base_array_layer;
        uint32_t layer_count;
    };

    // ioctl_create_image: in (2D, single mip, single layer, VK_SAMPLE_COUNT_1)
    struct create_image_request
    {
        object_id device;
        uint32_t format; // VkFormat
        uint32_t width;
        uint32_t height;
        uint32_t usage;  // VkImageUsageFlags
        uint32_t tiling; // VkImageTiling
        uint32_t reserved;
    };

    // ioctl_create_image: out
    struct create_image_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id image; // null_object on failure
    };

    // ioctl_destroy_image: in
    struct destroy_image_request
    {
        object_id device;
        object_id image;
    };

    // ioctl_get_image_memory_requirements: in (out = memory_requirements_response)
    struct get_image_memory_requirements_request
    {
        object_id device;
        object_id image;
    };

    // ioctl_bind_image_memory: in; out = result_response
    struct bind_image_memory_request
    {
        object_id device;
        object_id image;
        object_id memory;
        uint64_t offset; // VkDeviceSize
    };

    // ioctl_cmd_pipeline_barrier: records a single image memory barrier; out = result_response
    struct cmd_pipeline_barrier_request
    {
        object_id command_buffer;
        object_id image;
        image_subresource_range subresource;
        uint32_t src_stage_mask; // VkPipelineStageFlags
        uint32_t dst_stage_mask;
        uint32_t src_access_mask; // VkAccessFlags
        uint32_t dst_access_mask;
        uint32_t old_layout; // VkImageLayout
        uint32_t new_layout;
    };

    // ioctl_cmd_clear_color_image: records vkCmdClearColorImage (float clear color); out = result_response
    struct cmd_clear_color_image_request
    {
        object_id command_buffer;
        object_id image;
        image_subresource_range subresource;
        uint32_t image_layout; // VkImageLayout (current layout of the image)
        uint32_t reserved;
        float color_r; // RGBA float clear color (kept as named scalars to stay <cstdint>-only)
        float color_g;
        float color_b;
        float color_a;
    };

    // ioctl_cmd_copy_image_to_buffer: copies mip 0 / layer 0 of the image, tightly packed, to the
    // buffer at offset 0; out = result_response
    struct cmd_copy_image_to_buffer_request
    {
        object_id command_buffer;
        object_id image;
        object_id buffer;
        uint32_t image_layout; // VkImageLayout (current layout of the image)
        uint32_t width;
        uint32_t height;
        uint32_t aspect_mask; // VkImageAspectFlags
    };
}
