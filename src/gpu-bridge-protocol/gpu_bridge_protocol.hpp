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
}
