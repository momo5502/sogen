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
    inline constexpr uint32_t protocol_magic = 0x55504753; // 'SGPU'
    inline constexpr uint32_t protocol_version = 19;

    // Windows IOCTL encoding: CTL_CODE(DeviceType, Function, Method, Access).
    //   value = (DeviceType << 16) | (Access << 14) | (Function << 2) | Method
    // We use FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED (0) and FILE_ANY_ACCESS (0), and
    // keep functions in the vendor-defined range (>= 0x800), matching how a real driver
    // would lay out its private control codes.
    inline constexpr uint32_t device_type = 0x22; // FILE_DEVICE_UNKNOWN
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
        create_surface = 0x825,
        destroy_surface = 0x826,
        create_swapchain = 0x827,
        destroy_swapchain = 0x828,
        get_swapchain_images = 0x829,
        acquire_next_image = 0x82A,
        queue_present = 0x82B,
        create_shader_module = 0x82C,
        destroy_shader_module = 0x82D,
        create_image_view = 0x82E,
        destroy_image_view = 0x82F,
        create_render_pass = 0x830,
        destroy_render_pass = 0x831,
        create_framebuffer = 0x832,
        destroy_framebuffer = 0x833,
        create_pipeline_layout = 0x834,
        destroy_pipeline_layout = 0x835,
        create_graphics_pipeline = 0x836,
        destroy_pipeline = 0x837,
        cmd_begin_render_pass = 0x838,
        cmd_bind_pipeline = 0x839,
        cmd_draw = 0x83A,
        cmd_end_render_pass = 0x83B,
        cmd_push_constants = 0x83C,
        get_surface_capabilities = 0x83D,
        record_commands = 0x83E,
        cmd_bind_vertex_buffers = 0x83F,
        cmd_bind_index_buffer = 0x840,
        cmd_draw_indexed = 0x841,
        create_descriptor_set_layout = 0x842,
        destroy_descriptor_set_layout = 0x843,
        create_descriptor_pool = 0x844,
        destroy_descriptor_pool = 0x845,
        allocate_descriptor_sets = 0x846,
        update_descriptor_sets = 0x847,
        cmd_bind_descriptor_sets = 0x848,
        create_sampler = 0x849,
        destroy_sampler = 0x84A,
        cmd_copy_buffer_to_image = 0x84B,
        enumerate_device_extension_properties = 0x84C,
        get_physical_device_features2 = 0x84D,
        create_semaphore = 0x84E,
        destroy_semaphore = 0x84F,
        create_compute_pipeline = 0x850,
        get_physical_device_properties2 = 0x851,
        get_semaphore_counter_value = 0x852,
        signal_semaphore = 0x853,
        wait_semaphores = 0x854,
        get_buffer_device_address = 0x855,
        queue_submit2 = 0x856,
        get_physical_device_format_properties = 0x857,
        queue_wait_idle = 0x858,
        device_wait_idle = 0x859,
        reset_command_pool = 0x85A,
        reset_command_buffer = 0x85B,
        get_physical_device_image_format_properties = 0x85C,
        cmd_dispatch = 0x85D,
        cmd_dispatch_indirect = 0x85E,
        cmd_clear_depth_stencil_image = 0x85F,
        get_image_subresource_layout = 0x860,
        cmd_copy_buffer = 0x861,
        create_buffer_view = 0x862,
        destroy_buffer_view = 0x863,
        create_query_pool = 0x864,
        destroy_query_pool = 0x865,
        get_query_pool_results = 0x866,
        cmd_reset_query_pool = 0x867,
        cmd_begin_query = 0x868,
        cmd_end_query = 0x869,
        cmd_write_timestamp = 0x86A,
        reset_query_pool = 0x86B,
        cmd_bind_vertex_buffers2 = 0x86C,
        cmd_begin_rendering = 0x86D,
        cmd_end_rendering = 0x86E,
        cmd_execute_commands = 0x86F,
        cmd_set_viewport = 0x870,
        cmd_set_scissor = 0x871,
        cmd_set_depth_bias = 0x872,
        cmd_set_blend_constants = 0x873,
        cmd_set_depth_bounds = 0x874,
        cmd_set_line_width = 0x875,
        cmd_set_stencil = 0x876,
        cmd_set_stencil_op = 0x877,
        cmd_set_dynamic_u32 = 0x878,
        create_event = 0x879,
        destroy_event = 0x87A,
        get_event_status = 0x87B,
        set_event = 0x87C,
        reset_event = 0x87D,
        cmd_resolve_image = 0x87E,
        cmd_update_buffer = 0x87F,
    };

    // Discriminator for cmd_set_dynamic_u32: the family of extended-dynamic-state setters that all take a
    // single uint32 value (VkBool32 / enum). One wire command replaces ~a dozen near-identical vkCmdSet*.
    enum class dynamic_state_u32 : uint32_t
    {
        cull_mode = 0,
        front_face = 1,
        primitive_topology = 2,
        depth_test_enable = 3,
        depth_write_enable = 4,
        depth_compare_op = 5,
        depth_bounds_test_enable = 6,
        stencil_test_enable = 7,
        rasterizer_discard_enable = 8,
        depth_bias_enable = 9,
        primitive_restart_enable = 10,
    };

    // Discriminator for cmd_set_stencil: which of the three single-value stencil dynamic states to set.
    enum class stencil_dynamic_state : uint32_t
    {
        compare_mask = 0,
        write_mask = 1,
        reference = 2,
    };

    inline constexpr uint32_t ioctl_get_version = make_ioctl(static_cast<uint32_t>(command::get_version));
    inline constexpr uint32_t ioctl_create_instance = make_ioctl(static_cast<uint32_t>(command::create_instance));
    inline constexpr uint32_t ioctl_destroy_instance = make_ioctl(static_cast<uint32_t>(command::destroy_instance));
    inline constexpr uint32_t ioctl_enumerate_physical_devices = make_ioctl(static_cast<uint32_t>(command::enumerate_physical_devices));
    inline constexpr uint32_t ioctl_get_physical_device_properties =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_properties));
    inline constexpr uint32_t ioctl_get_queue_family_properties = make_ioctl(static_cast<uint32_t>(command::get_queue_family_properties));
    inline constexpr uint32_t ioctl_create_device = make_ioctl(static_cast<uint32_t>(command::create_device));
    inline constexpr uint32_t ioctl_get_device_queue = make_ioctl(static_cast<uint32_t>(command::get_device_queue));
    inline constexpr uint32_t ioctl_destroy_device = make_ioctl(static_cast<uint32_t>(command::destroy_device));
    inline constexpr uint32_t ioctl_create_command_pool = make_ioctl(static_cast<uint32_t>(command::create_command_pool));
    inline constexpr uint32_t ioctl_destroy_command_pool = make_ioctl(static_cast<uint32_t>(command::destroy_command_pool));
    inline constexpr uint32_t ioctl_allocate_command_buffer = make_ioctl(static_cast<uint32_t>(command::allocate_command_buffer));
    inline constexpr uint32_t ioctl_free_command_buffer = make_ioctl(static_cast<uint32_t>(command::free_command_buffer));
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
    inline constexpr uint32_t ioctl_download_memory = make_ioctl(static_cast<uint32_t>(command::download_memory));
    inline constexpr uint32_t ioctl_upload_memory = make_ioctl(static_cast<uint32_t>(command::upload_memory));
    inline constexpr uint32_t ioctl_create_image = make_ioctl(static_cast<uint32_t>(command::create_image));
    inline constexpr uint32_t ioctl_destroy_image = make_ioctl(static_cast<uint32_t>(command::destroy_image));
    inline constexpr uint32_t ioctl_get_image_memory_requirements =
        make_ioctl(static_cast<uint32_t>(command::get_image_memory_requirements));
    inline constexpr uint32_t ioctl_get_image_subresource_layout = make_ioctl(static_cast<uint32_t>(command::get_image_subresource_layout));
    inline constexpr uint32_t ioctl_bind_image_memory = make_ioctl(static_cast<uint32_t>(command::bind_image_memory));
    inline constexpr uint32_t ioctl_create_surface = make_ioctl(static_cast<uint32_t>(command::create_surface));
    inline constexpr uint32_t ioctl_destroy_surface = make_ioctl(static_cast<uint32_t>(command::destroy_surface));
    inline constexpr uint32_t ioctl_create_swapchain = make_ioctl(static_cast<uint32_t>(command::create_swapchain));
    inline constexpr uint32_t ioctl_destroy_swapchain = make_ioctl(static_cast<uint32_t>(command::destroy_swapchain));
    inline constexpr uint32_t ioctl_get_swapchain_images = make_ioctl(static_cast<uint32_t>(command::get_swapchain_images));
    inline constexpr uint32_t ioctl_acquire_next_image = make_ioctl(static_cast<uint32_t>(command::acquire_next_image));
    inline constexpr uint32_t ioctl_queue_present = make_ioctl(static_cast<uint32_t>(command::queue_present));
    inline constexpr uint32_t ioctl_create_shader_module = make_ioctl(static_cast<uint32_t>(command::create_shader_module));
    inline constexpr uint32_t ioctl_destroy_shader_module = make_ioctl(static_cast<uint32_t>(command::destroy_shader_module));
    inline constexpr uint32_t ioctl_create_image_view = make_ioctl(static_cast<uint32_t>(command::create_image_view));
    inline constexpr uint32_t ioctl_destroy_image_view = make_ioctl(static_cast<uint32_t>(command::destroy_image_view));
    inline constexpr uint32_t ioctl_create_buffer_view = make_ioctl(static_cast<uint32_t>(command::create_buffer_view));
    inline constexpr uint32_t ioctl_destroy_buffer_view = make_ioctl(static_cast<uint32_t>(command::destroy_buffer_view));
    inline constexpr uint32_t ioctl_create_query_pool = make_ioctl(static_cast<uint32_t>(command::create_query_pool));
    inline constexpr uint32_t ioctl_destroy_query_pool = make_ioctl(static_cast<uint32_t>(command::destroy_query_pool));
    inline constexpr uint32_t ioctl_get_query_pool_results = make_ioctl(static_cast<uint32_t>(command::get_query_pool_results));
    inline constexpr uint32_t ioctl_reset_query_pool = make_ioctl(static_cast<uint32_t>(command::reset_query_pool));
    inline constexpr uint32_t ioctl_create_render_pass = make_ioctl(static_cast<uint32_t>(command::create_render_pass));
    inline constexpr uint32_t ioctl_destroy_render_pass = make_ioctl(static_cast<uint32_t>(command::destroy_render_pass));
    inline constexpr uint32_t ioctl_create_framebuffer = make_ioctl(static_cast<uint32_t>(command::create_framebuffer));
    inline constexpr uint32_t ioctl_destroy_framebuffer = make_ioctl(static_cast<uint32_t>(command::destroy_framebuffer));
    inline constexpr uint32_t ioctl_create_pipeline_layout = make_ioctl(static_cast<uint32_t>(command::create_pipeline_layout));
    inline constexpr uint32_t ioctl_destroy_pipeline_layout = make_ioctl(static_cast<uint32_t>(command::destroy_pipeline_layout));
    inline constexpr uint32_t ioctl_create_graphics_pipeline = make_ioctl(static_cast<uint32_t>(command::create_graphics_pipeline));
    inline constexpr uint32_t ioctl_destroy_pipeline = make_ioctl(static_cast<uint32_t>(command::destroy_pipeline));
    inline constexpr uint32_t ioctl_get_surface_capabilities = make_ioctl(static_cast<uint32_t>(command::get_surface_capabilities));
    inline constexpr uint32_t ioctl_record_commands = make_ioctl(static_cast<uint32_t>(command::record_commands));
    inline constexpr uint32_t ioctl_create_descriptor_set_layout = make_ioctl(static_cast<uint32_t>(command::create_descriptor_set_layout));
    inline constexpr uint32_t ioctl_destroy_descriptor_set_layout =
        make_ioctl(static_cast<uint32_t>(command::destroy_descriptor_set_layout));
    inline constexpr uint32_t ioctl_create_descriptor_pool = make_ioctl(static_cast<uint32_t>(command::create_descriptor_pool));
    inline constexpr uint32_t ioctl_destroy_descriptor_pool = make_ioctl(static_cast<uint32_t>(command::destroy_descriptor_pool));
    inline constexpr uint32_t ioctl_allocate_descriptor_sets = make_ioctl(static_cast<uint32_t>(command::allocate_descriptor_sets));
    inline constexpr uint32_t ioctl_update_descriptor_sets = make_ioctl(static_cast<uint32_t>(command::update_descriptor_sets));
    inline constexpr uint32_t ioctl_create_sampler = make_ioctl(static_cast<uint32_t>(command::create_sampler));
    inline constexpr uint32_t ioctl_destroy_sampler = make_ioctl(static_cast<uint32_t>(command::destroy_sampler));
    inline constexpr uint32_t ioctl_enumerate_device_extension_properties =
        make_ioctl(static_cast<uint32_t>(command::enumerate_device_extension_properties));
    inline constexpr uint32_t ioctl_get_physical_device_features2 =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_features2));
    inline constexpr uint32_t ioctl_create_semaphore = make_ioctl(static_cast<uint32_t>(command::create_semaphore));
    inline constexpr uint32_t ioctl_destroy_semaphore = make_ioctl(static_cast<uint32_t>(command::destroy_semaphore));
    inline constexpr uint32_t ioctl_create_compute_pipeline = make_ioctl(static_cast<uint32_t>(command::create_compute_pipeline));
    inline constexpr uint32_t ioctl_get_physical_device_properties2 =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_properties2));
    inline constexpr uint32_t ioctl_get_semaphore_counter_value = make_ioctl(static_cast<uint32_t>(command::get_semaphore_counter_value));
    inline constexpr uint32_t ioctl_signal_semaphore = make_ioctl(static_cast<uint32_t>(command::signal_semaphore));
    inline constexpr uint32_t ioctl_wait_semaphores = make_ioctl(static_cast<uint32_t>(command::wait_semaphores));
    inline constexpr uint32_t ioctl_get_buffer_device_address = make_ioctl(static_cast<uint32_t>(command::get_buffer_device_address));
    inline constexpr uint32_t ioctl_queue_submit2 = make_ioctl(static_cast<uint32_t>(command::queue_submit2));
    inline constexpr uint32_t ioctl_get_physical_device_format_properties =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_format_properties));
    inline constexpr uint32_t ioctl_queue_wait_idle = make_ioctl(static_cast<uint32_t>(command::queue_wait_idle));
    inline constexpr uint32_t ioctl_device_wait_idle = make_ioctl(static_cast<uint32_t>(command::device_wait_idle));
    inline constexpr uint32_t ioctl_reset_command_pool = make_ioctl(static_cast<uint32_t>(command::reset_command_pool));
    inline constexpr uint32_t ioctl_reset_command_buffer = make_ioctl(static_cast<uint32_t>(command::reset_command_buffer));
    inline constexpr uint32_t ioctl_get_physical_device_image_format_properties =
        make_ioctl(static_cast<uint32_t>(command::get_physical_device_image_format_properties));
    inline constexpr uint32_t ioctl_create_event = make_ioctl(static_cast<uint32_t>(command::create_event));
    inline constexpr uint32_t ioctl_destroy_event = make_ioctl(static_cast<uint32_t>(command::destroy_event));
    inline constexpr uint32_t ioctl_get_event_status = make_ioctl(static_cast<uint32_t>(command::get_event_status));
    inline constexpr uint32_t ioctl_set_event = make_ioctl(static_cast<uint32_t>(command::set_event));
    inline constexpr uint32_t ioctl_reset_event = make_ioctl(static_cast<uint32_t>(command::reset_event));
    inline constexpr uint32_t ioctl_cmd_resolve_image = make_ioctl(static_cast<uint32_t>(command::cmd_resolve_image));
    inline constexpr uint32_t ioctl_cmd_update_buffer = make_ioctl(static_cast<uint32_t>(command::cmd_update_buffer));

    // Opaque identifier handed to the guest in place of a host Vulkan handle. The host keeps the
    // real VkInstance / VkPhysicalDevice / ... in a table and the guest only ever sees this id, so
    // raw host pointers never cross the boundary.
    using object_id = uint64_t;

    inline constexpr object_id null_object = 0;

    // Real Vulkan structs (VkPhysicalDeviceProperties, ...) ride the wire as raw bytes; both sides
    // agree on their layout through their own <vulkan/vulkan_core.h>. This keeps the protocol header
    // dependency-free while still passing native structures unchanged.

    struct version_response
    {
        uint32_t magic;   // protocol_magic
        uint32_t version; // protocol_version
    };

    struct create_instance_response
    {
        int32_t vk_result;  // VkResult from the host
        uint32_t reserved;  // padding / future flags
        object_id instance; // null_object on failure
    };

    struct destroy_instance_request
    {
        object_id instance;
    };

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

    struct enumerate_device_extension_properties_request
    {
        object_id physical_device;
        uint32_t max_count; // capacity (entries) of the VkExtensionProperties array after the response header
        uint32_t reserved;
    };

    // ioctl_enumerate_device_extension_properties: out header, followed by `count` raw VkExtensionProperties
    struct enumerate_device_extension_properties_response
    {
        int32_t vk_result;
        uint32_t count; // true number of device extensions on the device
    };

    // One serialized pNext feature struct: the {sType, pNext} header is dropped and only the run of
    // VkBool32 toggles after it crosses the wire. Shared by features2 queries and create_device.
    struct feature_chain_record
    {
        uint32_t s_type;    // VkStructureType
        uint32_t body_size; // bytes of feature-bool body following this header (0 if the sType is unknown to the host)
    };

    // ioctl_get_physical_device_features2: in header, followed by `struct_count` uint32 sType values
    // (the sTypes of the pNext structs the caller chained onto VkPhysicalDeviceFeatures2).
    struct get_physical_device_features2_request
    {
        object_id physical_device;
        uint32_t struct_count;
        uint32_t reserved;
    };

    // ioctl_get_physical_device_features2: out header, followed by `struct_count` records, each a
    // feature_chain_record + body_size bytes, in the same order as the requested sTypes.
    struct get_physical_device_features2_response
    {
        int32_t vk_result;
        uint32_t struct_count;
    };

    // ioctl_get_physical_device_properties2: same chain convention as features2, but for the property
    // structs chained onto VkPhysicalDeviceProperties2 (the base VkPhysicalDeviceProperties is fetched
    // separately via ioctl_get_physical_device_properties; only the pNext structs travel here).
    struct get_physical_device_properties2_request
    {
        object_id physical_device;
        uint32_t struct_count;
        uint32_t reserved;
    };

    struct get_physical_device_properties2_response
    {
        int32_t vk_result;
        uint32_t struct_count;
    };

    // ioctl_create_device: in (single queue from one family). The header is followed by:
    //   (a) `extension_count` NUL-terminated extension-name strings (extension_blob_size bytes), then
    //   (b) `feature_struct_count` feature_chain_record + body entries (feature_blob_size bytes) -- the
    //       same chain format as get_physical_device_features2, carrying the features to enable
    //       (a VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 record holds the base VkPhysicalDeviceFeatures).
    // One requested queue family in a vkCreateDevice call. The wire carries `queue_create_count` of these
    // immediately after create_device_request, before the extension and feature blobs.
    struct device_queue_create_entry
    {
        uint32_t queue_family_index;
        uint32_t queue_count;
    };

    struct create_device_request
    {
        object_id physical_device;
        uint32_t queue_create_count;
        uint32_t extension_count;
        uint32_t extension_blob_size;
        uint32_t feature_struct_count;
        uint32_t feature_blob_size;
        uint32_t reserved;
    };

    struct create_device_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id device; // null_object on failure
    };

    struct get_device_queue_request
    {
        object_id device;
        uint32_t queue_family_index;
        uint32_t queue_index;
    };

    struct get_device_queue_response
    {
        object_id queue; // null_object on failure
    };

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

    struct create_command_pool_request
    {
        object_id device;
        uint32_t queue_family_index;
        uint32_t flags;
    };

    struct create_command_pool_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id command_pool;
    };

    struct destroy_command_pool_request
    {
        object_id device;
        object_id command_pool;
    };

    struct allocate_command_buffer_request
    {
        object_id device;
        object_id command_pool;
        uint32_t level; // VkCommandBufferLevel (0 = primary, 1 = secondary)
        uint32_t reserved;
    };

    struct allocate_command_buffer_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id command_buffer;
    };

    struct free_command_buffer_request
    {
        object_id device;
        object_id command_pool;
        object_id command_buffer;
    };

    // For a secondary command buffer recorded inside dynamic rendering, the begin carries the inheritance
    // rendering info (immediately followed by `inherit_color_count` uint32 VkFormat color formats).
    struct begin_command_buffer_request
    {
        object_id command_buffer;
        uint32_t flags;
        uint32_t is_secondary;
        uint32_t inherit_view_mask;
        uint32_t inherit_color_count;
        uint32_t inherit_depth_format;          // VkFormat
        uint32_t inherit_stencil_format;        // VkFormat
        uint32_t inherit_rasterization_samples; // VkSampleCountFlagBits
        uint32_t inherit_rendering_flags;       // VkRenderingFlags
        // uint32_t inherit_color_formats[inherit_color_count];
    };

    struct end_command_buffer_request
    {
        object_id command_buffer;
    };

    struct create_fence_request
    {
        object_id device;
        uint32_t flags;
        uint32_t reserved;
    };

    struct create_fence_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id fence;
    };

    struct destroy_fence_request
    {
        object_id device;
        object_id fence;
    };

    struct create_semaphore_request
    {
        object_id device;
        uint32_t flags;
        uint32_t semaphore_type; // VkSemaphoreType: 0 = binary, 1 = timeline
        uint64_t initial_value;  // timeline initial value (ignored for binary)
    };

    struct create_semaphore_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id semaphore;
    };

    struct destroy_semaphore_request
    {
        object_id device;
        object_id semaphore;
    };

    struct get_semaphore_counter_value_request
    {
        object_id device;
        object_id semaphore;
    };

    struct get_semaphore_counter_value_response
    {
        int32_t vk_result;
        uint32_t reserved;
        uint64_t value;
    };

    struct signal_semaphore_request
    {
        object_id device;
        object_id semaphore;
        uint64_t value;
    };

    // wait_semaphores: header followed by `semaphore_count` { object_id semaphore; uint64_t value; } pairs.
    struct wait_semaphores_request
    {
        object_id device;
        uint32_t flags; // VkSemaphoreWaitFlags
        uint32_t semaphore_count;
        uint64_t timeout;
    };

    struct wait_semaphore_entry
    {
        object_id semaphore;
        uint64_t value;
    };

    struct get_buffer_device_address_request
    {
        object_id device;
        object_id buffer;
    };

    struct get_buffer_device_address_response
    {
        uint64_t address;
    };

    // queue_submit2: header followed by `wait_count` submit2_semaphore_entry, then `command_buffer_count`
    // object_id command buffers, then `signal_count` submit2_semaphore_entry. Mirrors VkSubmitInfo2.
    struct queue_submit2_request
    {
        object_id queue;
        object_id fence; // 0 = none
        uint32_t wait_count;
        uint32_t command_buffer_count;
        uint32_t signal_count;
        uint32_t reserved;
    };

    struct submit2_semaphore_entry
    {
        object_id semaphore;
        uint64_t value;      // timeline value (ignored for binary semaphores)
        uint64_t stage_mask; // VkPipelineStageFlags2
    };

    struct get_physical_device_format_properties_request
    {
        object_id physical_device;
        uint32_t format; // VkFormat
        uint32_t reserved;
    };

    // Real VkFormatProperties feature flags for the queried format.
    struct get_physical_device_format_properties_response
    {
        uint32_t linear_tiling_features;
        uint32_t optimal_tiling_features;
        uint32_t buffer_features;
        uint32_t reserved;
    };

    struct reset_fence_request
    {
        object_id device;
        object_id fence;
    };

    // VK_NOT_READY (unsignaled). This is the non-blocking poll the guest spins on while yielding, so
    // the host thread is never blocked on the GPU.
    struct get_fence_status_request
    {
        object_id fence;
    };

    // VkEvent (host-set GPU event). create returns a handle; status/set/reset return a plain vk_result
    // (get returns VK_EVENT_SET / VK_EVENT_RESET, the non-blocking poll DXVK spins on).
    struct create_event_request
    {
        object_id device;
        uint32_t flags; // VkEventCreateFlags
        uint32_t reserved;
    };

    struct create_event_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id event;
    };

    struct destroy_event_request
    {
        object_id device;
        object_id event;
    };

    struct event_op_request
    {
        object_id device;
        object_id event;
    };

    struct get_event_status_request
    {
        object_id event;
    };

    struct queue_submit_request
    {
        object_id queue;
        object_id command_buffer;
        object_id fence; // null_object for no fence
    };

    struct queue_wait_idle_request
    {
        object_id queue;
    };

    struct device_wait_idle_request
    {
        object_id device;
    };

    struct reset_command_pool_request
    {
        object_id device;
        object_id command_pool;
        uint32_t flags; // VkCommandPoolResetFlags
        uint32_t reserved;
    };

    struct reset_command_buffer_request
    {
        object_id command_buffer;
        uint32_t flags; // VkCommandBufferResetFlags
        uint32_t reserved;
    };

    struct get_physical_device_image_format_properties_request
    {
        object_id physical_device;
        uint32_t format; // VkFormat
        uint32_t type;   // VkImageType
        uint32_t tiling; // VkImageTiling
        uint32_t usage;  // VkImageUsageFlags
        uint32_t flags;  // VkImageCreateFlags
        uint32_t reserved;
    };

    // Mirrors VkImageFormatProperties with a fixed cross-bitness layout (maxResourceSize 8-byte aligned).
    struct get_physical_device_image_format_properties_response
    {
        int32_t vk_result;
        uint32_t max_mip_levels;
        uint32_t max_array_layers;
        uint32_t sample_counts; // VkSampleCountFlags
        uint32_t max_extent_width;
        uint32_t max_extent_height;
        uint32_t max_extent_depth;
        uint32_t reserved;
        uint64_t max_resource_size; // VkDeviceSize
    };

    // ioctl_get_physical_device_memory_properties: in (out = raw VkPhysicalDeviceMemoryProperties bytes)
    struct get_physical_device_memory_properties_request
    {
        object_id physical_device;
    };

    struct allocate_memory_request
    {
        object_id device;
        uint64_t size; // VkDeviceSize allocationSize
        uint32_t memory_type_index;
        uint32_t reserved;
    };

    struct allocate_memory_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id memory; // null_object on failure
    };

    struct free_memory_request
    {
        object_id device;
        object_id memory;
    };

    struct create_buffer_request
    {
        object_id device;
        uint64_t size; // VkDeviceSize
        uint32_t usage;
        uint32_t reserved;
    };

    struct create_buffer_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id buffer; // null_object on failure
    };

    struct destroy_buffer_request
    {
        object_id device;
        object_id buffer;
    };

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

    struct bind_buffer_memory_request
    {
        object_id device;
        object_id buffer;
        object_id memory;
        uint64_t offset; // VkDeviceSize
    };

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

    // ioctl_upload_memory: in header immediately followed by `size` raw bytes;
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

    struct create_image_request
    {
        object_id device;
        uint32_t format; // VkFormat
        uint32_t width;
        uint32_t height;
        uint32_t usage;   // VkImageUsageFlags
        uint32_t tiling;  // VkImageTiling
        uint32_t samples; // VkSampleCountFlagBits (1 = no MSAA)
    };

    struct create_image_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id image; // null_object on failure
    };

    struct destroy_image_request
    {
        object_id device;
        object_id image;
    };

    struct get_image_memory_requirements_request
    {
        object_id device;
        object_id image;
    };

    struct get_image_subresource_layout_request
    {
        object_id device;
        object_id image;
        uint32_t aspect_mask; // VkImageAspectFlags
        uint32_t mip_level;
        uint32_t array_layer;
        uint32_t reserved;
    };

    struct get_image_subresource_layout_response
    {
        int32_t vk_result;
        uint32_t reserved;
        uint64_t offset;      // VkSubresourceLayout::offset (all VkDeviceSize, ABI-identical)
        uint64_t size;        // VkSubresourceLayout::size
        uint64_t row_pitch;   // VkSubresourceLayout::rowPitch
        uint64_t array_pitch; // VkSubresourceLayout::arrayPitch
        uint64_t depth_pitch; // VkSubresourceLayout::depthPitch
    };

    struct bind_image_memory_request
    {
        object_id device;
        object_id image;
        object_id memory;
        uint64_t offset; // VkDeviceSize
    };

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

    struct cmd_clear_depth_stencil_image_request
    {
        object_id command_buffer;
        object_id image;
        image_subresource_range subresource;
        uint32_t image_layout; // VkImageLayout (current layout of the image)
        uint32_t stencil;      // VkClearDepthStencilValue::stencil
        float depth;           // VkClearDepthStencilValue::depth
        uint32_t reserved;
        uint32_t reserved2;
    };

    // the buffer at offset 0;
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

    // buffer (offset 0) into mip 0 / layer 0 of the image.
    struct cmd_copy_buffer_to_image_request
    {
        object_id command_buffer;
        object_id buffer;
        object_id image;
        uint32_t image_layout; // VkImageLayout (current dst layout, e.g. TRANSFER_DST_OPTIMAL)
        uint32_t width;
        uint32_t height;
        uint32_t aspect_mask; // VkImageAspectFlags
    };

    // Resolves a multisampled source image into a single-sample destination (full image, mip 0 / layer 0).
    // DXVK uses this to resolve an MSAA backbuffer before presentation.
    struct cmd_resolve_image_request
    {
        object_id command_buffer;
        object_id src_image;
        object_id dst_image;
        uint32_t src_layout; // VkImageLayout
        uint32_t dst_layout; // VkImageLayout
        uint32_t width;
        uint32_t height;
        uint32_t aspect_mask; // VkImageAspectFlags
    };

    // Updates a buffer region inline from data that trails this header in the wire stream (vkCmdUpdateBuffer).
    struct cmd_update_buffer_request
    {
        object_id command_buffer;
        object_id buffer;
        uint64_t offset;
        uint32_t size;
        uint32_t reserved;
    };

    struct create_sampler_request
    {
        object_id device;
        uint32_t mag_filter;     // VkFilter
        uint32_t min_filter;     // VkFilter
        uint32_t address_mode_u; // VkSamplerAddressMode
        uint32_t address_mode_v;
        uint32_t address_mode_w;
        uint32_t reserved;
    };

    struct create_surface_request
    {
        uint64_t hwnd;
    };

    struct create_surface_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id surface;
    };

    struct destroy_surface_request
    {
        object_id surface;
    };

    struct create_swapchain_request
    {
        object_id device;
        object_id surface;
        uint32_t format; // VkFormat (use B8G8R8A8_UNORM to match the UI backend's bgra8)
        uint32_t width;
        uint32_t height;
        uint32_t min_image_count;
        uint32_t image_usage;  // VkImageUsageFlags requested on swapchain images
        uint32_t present_mode; // VkPresentModeKHR (ignored; effectively FIFO)
    };

    struct create_swapchain_response
    {
        int32_t vk_result;
        uint32_t image_count; // number of images actually created
        object_id swapchain;
    };

    struct destroy_swapchain_request
    {
        object_id device;
        object_id swapchain;
    };

    struct get_swapchain_images_request
    {
        object_id swapchain;
        uint32_t max_count; // capacity of the object_id array following the response header
        uint32_t reserved;
    };

    // ioctl_get_swapchain_images: out header, immediately followed by `count` object_id image entries
    struct get_swapchain_images_response
    {
        int32_t vk_result;
        uint32_t count;
        // object_id images[count];
    };

    struct acquire_next_image_request
    {
        object_id swapchain;
        object_id semaphore; // null_object if the caller passed VK_NULL_HANDLE
        object_id fence;     // null_object if the caller passed VK_NULL_HANDLE
    };

    struct acquire_next_image_response
    {
        int32_t vk_result;
        uint32_t image_index;
    };

    // bridge hands the pixels to the guest window via the UI backend.
    struct queue_present_request
    {
        object_id queue;
        object_id swapchain;
        uint32_t image_index;
        uint32_t reserved;
    };

    // Shared output for the "create a device child" commands below (one new object id).
    struct object_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id object; // null_object on failure
    };

    // Shared input for the matching "destroy a device child" commands (vkDestroyX(device, child)).
    struct device_child_request
    {
        object_id device;
        object_id object;
    };

    // ioctl_create_shader_module: in header immediately followed by `code_size` bytes of SPIR-V;
    // out = object_response
    struct create_shader_module_request
    {
        object_id device;
        uint32_t code_size; // bytes (multiple of 4)
        uint32_t reserved;
        // uint8_t code[code_size];
    };

    struct create_image_view_request
    {
        object_id device;
        object_id image;
        uint32_t format;      // VkFormat
        uint32_t aspect_mask; // VkImageAspectFlags (COLOR for color targets, DEPTH for depth); 0 => COLOR
    };

    // out = object_response (a VkBufferView)
    struct create_buffer_view_request
    {
        object_id device;
        object_id buffer;
        uint32_t format; // VkFormat
        uint32_t reserved;
        uint64_t offset; // VkDeviceSize
        uint64_t range;  // VkDeviceSize (VK_WHOLE_SIZE allowed)
    };

    // One source/destination/size triple of a buffer-to-buffer copy (trailing element of cmd_copy_buffer).
    struct buffer_copy_region
    {
        uint64_t src_offset; // VkDeviceSize
        uint64_t dst_offset; // VkDeviceSize
        uint64_t size;       // VkDeviceSize
    };

    // record payload (command::cmd_copy_buffer): header immediately followed by `region_count` buffer_copy_region entries.
    struct cmd_copy_buffer_request
    {
        object_id command_buffer;
        object_id src_buffer;
        object_id dst_buffer;
        uint32_t region_count;
        uint32_t reserved;
    };

    // out = object_response (a VkQueryPool)
    struct create_query_pool_request
    {
        object_id device;
        uint32_t query_type;          // VkQueryType
        uint32_t query_count;         // number of queries in the pool
        uint32_t pipeline_statistics; // VkQueryPipelineStatisticFlags (for PIPELINE_STATISTICS pools)
        uint32_t reserved;
    };

    // ioctl_get_query_pool_results: out = get_query_pool_results_response header followed by `data_size` result bytes.
    struct get_query_pool_results_request
    {
        object_id device;
        object_id query_pool;
        uint32_t first_query;
        uint32_t query_count;
        uint32_t data_size; // bytes of result buffer the caller provided
        uint32_t stride;    // VkDeviceSize stride between results (fits 32-bit for query results)
        uint32_t flags;     // VkQueryResultFlags
        uint32_t reserved;
    };

    struct get_query_pool_results_response
    {
        int32_t vk_result;
        uint32_t data_size; // bytes actually written
        // uint8_t data[data_size];
    };

    struct cmd_reset_query_pool_request
    {
        object_id command_buffer;
        object_id query_pool;
        uint32_t first_query;
        uint32_t query_count;
    };

    // ioctl_reset_query_pool: host-side query reset (no command buffer); out = result_response
    struct reset_query_pool_request
    {
        object_id device;
        object_id query_pool;
        uint32_t first_query;
        uint32_t query_count;
    };

    struct cmd_begin_query_request
    {
        object_id command_buffer;
        object_id query_pool;
        uint32_t query;
        uint32_t flags; // VkQueryControlFlags
    };

    struct cmd_end_query_request
    {
        object_id command_buffer;
        object_id query_pool;
        uint32_t query;
        uint32_t reserved;
    };

    struct cmd_write_timestamp_request
    {
        object_id command_buffer;
        object_id query_pool;
        uint32_t query;
        uint32_t pipeline_stage; // VkPipelineStageFlagBits
    };

    // out = object_response
    struct create_render_pass_request
    {
        object_id device;
        uint32_t format;         // VkFormat of the color attachment
        uint32_t load_op;        // VkAttachmentLoadOp
        uint32_t store_op;       // VkAttachmentStoreOp
        uint32_t initial_layout; // VkImageLayout
        uint32_t final_layout;   // VkImageLayout (PRESENT_SRC_KHR is mapped to TRANSFER_SRC_OPTIMAL)
        uint32_t depth_format;   // VkFormat of the depth attachment, or 0 for no depth attachment
        uint32_t reserved;
    };

    struct create_framebuffer_request
    {
        object_id device;
        object_id render_pass;
        object_id image_view; // color attachment
        object_id depth_view; // depth attachment, or null_object for none
        uint32_t width;
        uint32_t height;
    };

    // variable-length list of descriptor-set-layout ids: this header is immediately followed by
    // `set_layout_count` object_id entries). out = object_response
    struct create_pipeline_layout_request
    {
        object_id device;
        uint32_t push_constant_stages; // VkShaderStageFlags (0 = no push constants)
        uint32_t push_constant_size;   // bytes, from offset 0
        uint32_t set_layout_count;     // number of object_id set-layout ids that follow
        uint32_t reserved;
        // object_id set_layouts[set_layout_count];
    };

    // A VkVertexInputBindingDescription flattened to plain integers.
    struct vertex_input_binding
    {
        uint32_t binding;
        uint32_t stride;
        uint32_t input_rate; // VkVertexInputRate
        uint32_t reserved;
    };

    // A VkVertexInputAttributeDescription flattened to plain integers.
    struct vertex_input_attribute
    {
        uint32_t location;
        uint32_t binding;
        uint32_t format; // VkFormat
        uint32_t offset;
    };

    // color attachment). The vertex input state is variable-length: the input buffer is this header
    // immediately followed by `binding_count` vertex_input_binding entries and then `attribute_count`
    // vertex_input_attribute entries. Both counts 0 => no vertex input (vertices baked into the shader).
    // out = object_response
    inline constexpr uint32_t max_color_attachments = 8;

    struct create_graphics_pipeline_request
    {
        object_id device;
        object_id render_pass; // 0 => dynamic rendering: use the attachment formats below, and viewport/scissor are dynamic
        object_id pipeline_layout;
        object_id vertex_shader;
        object_id fragment_shader;
        uint32_t width;
        uint32_t height;
        uint32_t depth_test_enable;  // VkBool32 (0 => no depth-stencil state, as before)
        uint32_t depth_write_enable; // VkBool32
        uint32_t depth_compare_op;   // VkCompareOp (used when depth_test_enable != 0)
        // Dynamic-rendering attachment formats (used when render_pass == 0). DXVK 2.x builds pipelines with
        // VK_KHR_dynamic_rendering (renderPass = VK_NULL_HANDLE + VkPipelineRenderingCreateInfo) instead of a
        // render-pass object, so the host rebuilds that info from these formats.
        uint32_t color_attachment_count;               // number of valid entries in color_formats
        uint32_t depth_format;                         // VkFormat for depth (VK_FORMAT_UNDEFINED => none)
        uint32_t stencil_format;                       // VkFormat for stencil (VK_FORMAT_UNDEFINED => none)
        uint32_t color_formats[max_color_attachments]; // VkFormat per color attachment
        uint32_t binding_count;                        // number of vertex_input_binding entries that follow
        uint32_t attribute_count;                      // number of vertex_input_attribute entries that follow the bindings
        // vertex_input_binding bindings[binding_count];
        // vertex_input_attribute attributes[attribute_count];
    };

    struct create_compute_pipeline_request
    {
        object_id device;
        object_id pipeline_layout;
        object_id shader_module;
    };

    struct create_compute_pipeline_response
    {
        int32_t vk_result;
        uint32_t reserved;
        object_id pipeline;
    };

    // plus a depth clear used only when the render pass has a depth attachment).
    struct cmd_begin_render_pass_request
    {
        object_id command_buffer;
        object_id render_pass;
        object_id framebuffer;
        uint32_t width;
        uint32_t height;
        float clear_r;
        float clear_g;
        float clear_b;
        float clear_a;
        float clear_depth;
        uint32_t reserved;
    };

    struct cmd_bind_pipeline_request
    {
        object_id command_buffer;
        object_id pipeline;
        uint32_t bind_point; // VkPipelineBindPoint (0 = graphics, 1 = compute)
        uint32_t reserved;
    };

    struct cmd_dispatch_request
    {
        object_id command_buffer;
        uint32_t group_count_x;
        uint32_t group_count_y;
        uint32_t group_count_z;
        uint32_t reserved;
    };

    struct cmd_dispatch_indirect_request
    {
        object_id command_buffer;
        object_id buffer;
        uint64_t offset; // VkDeviceSize
    };

    struct cmd_draw_request
    {
        object_id command_buffer;
        uint32_t vertex_count;
        uint32_t instance_count;
        uint32_t first_vertex;
        uint32_t first_instance;
    };

    // One bound vertex buffer (trailing-array element of cmd_bind_vertex_buffers_request).
    struct vertex_buffer_binding
    {
        object_id buffer;
        uint64_t offset; // VkDeviceSize
    };

    // immediately followed by `binding_count` vertex_buffer_binding entries.
    struct cmd_bind_vertex_buffers_request
    {
        object_id command_buffer;
        uint32_t first_binding;
        uint32_t binding_count;
        // vertex_buffer_binding bindings[binding_count];
    };

    // One bound vertex buffer for vkCmdBindVertexBuffers2 (carries the dynamic size + stride DXVK sets via
    // extendedDynamicState). size/stride VK_WHOLE_SIZE/0 mean "use the whole buffer / pipeline stride".
    struct vertex_buffer_binding2
    {
        object_id buffer;
        uint64_t offset; // VkDeviceSize
        uint64_t size;   // VkDeviceSize (VK_WHOLE_SIZE if not provided)
        uint64_t stride; // VkDeviceSize
    };

    // immediately followed by `binding_count` vertex_buffer_binding2 entries.
    struct cmd_bind_vertex_buffers2_request
    {
        object_id command_buffer;
        uint32_t first_binding;
        uint32_t binding_count;
        uint32_t has_sizes;   // 1 if the size field is meaningful
        uint32_t has_strides; // 1 if the stride field is meaningful
    };

    // One VkRenderingAttachmentInfo (dynamic rendering). image_view 0 = unused (e.g. absent depth/stencil).
    struct rendering_attachment
    {
        object_id image_view;         // VkImageView
        object_id resolve_image_view; // 0 = no resolve
        uint32_t image_layout;        // VkImageLayout
        uint32_t resolve_image_layout;
        uint32_t resolve_mode; // VkResolveModeFlagBits
        uint32_t load_op;      // VkAttachmentLoadOp
        uint32_t store_op;     // VkAttachmentStoreOp
        uint32_t reserved;
        uint32_t clear_value[4]; // VkClearValue (16 bytes; color float/int/uint, or {depth(float bits), stencil})
    };

    // record payload (command::cmd_begin_rendering): header followed by `color_attachment_count` color
    // rendering_attachment entries, then (if has_depth) one depth entry, then (if has_stencil) one stencil entry.
    struct cmd_begin_rendering_request
    {
        object_id command_buffer;
        int32_t render_area_x;
        int32_t render_area_y;
        uint32_t render_area_width;
        uint32_t render_area_height;
        uint32_t layer_count;
        uint32_t view_mask;
        uint32_t color_attachment_count;
        uint32_t has_depth;
        uint32_t has_stencil;
        uint32_t flags; // VkRenderingFlags
        // rendering_attachment color[color_attachment_count]; [depth]; [stencil];
    };

    struct cmd_end_rendering_request
    {
        object_id command_buffer;
    };

    // record payload (command::cmd_execute_commands): header followed by `count` object_id secondary
    // command-buffer ids to execute from this (primary) command buffer.
    struct cmd_execute_commands_request
    {
        object_id command_buffer;
        uint32_t count;
        uint32_t reserved;
    };

    // One VkViewport. Trails cmd_set_viewport_request `count` times.
    struct viewport_entry
    {
        float x;
        float y;
        float width;
        float height;
        float min_depth;
        float max_depth;
    };

    // cmd_set_viewport: with_count!=0 selects vkCmdSetViewportWithCount (first ignored), else vkCmdSetViewport.
    struct cmd_set_viewport_request
    {
        object_id command_buffer;
        uint32_t first;
        uint32_t count;
        uint32_t with_count;
        uint32_t reserved;
    };

    // One VkRect2D. Trails cmd_set_scissor_request `count` times.
    struct scissor_entry
    {
        int32_t offset_x;
        int32_t offset_y;
        uint32_t width;
        uint32_t height;
    };

    // cmd_set_scissor: with_count!=0 selects vkCmdSetScissorWithCount (first ignored), else vkCmdSetScissor.
    struct cmd_set_scissor_request
    {
        object_id command_buffer;
        uint32_t first;
        uint32_t count;
        uint32_t with_count;
        uint32_t reserved;
    };

    struct cmd_set_depth_bias_request
    {
        object_id command_buffer;
        float constant_factor;
        float clamp;
        float slope_factor;
        uint32_t reserved;
    };

    struct cmd_set_blend_constants_request
    {
        object_id command_buffer;
        float constants[4];
    };

    struct cmd_set_depth_bounds_request
    {
        object_id command_buffer;
        float min_depth_bounds;
        float max_depth_bounds;
    };

    struct cmd_set_line_width_request
    {
        object_id command_buffer;
        float line_width;
        uint32_t reserved;
    };

    // cmd_set_stencil: `which` is a stencil_dynamic_state value (compare mask / write mask / reference).
    struct cmd_set_stencil_request
    {
        object_id command_buffer;
        uint32_t which;
        uint32_t face_mask;
        uint32_t value;
        uint32_t reserved;
    };

    struct cmd_set_stencil_op_request
    {
        object_id command_buffer;
        uint32_t face_mask;
        uint32_t fail_op;
        uint32_t pass_op;
        uint32_t depth_fail_op;
        uint32_t compare_op;
        uint32_t reserved;
    };

    // cmd_set_dynamic_u32: `state` is a dynamic_state_u32 value; `value` is the VkBool32/enum to set.
    struct cmd_set_dynamic_u32_request
    {
        object_id command_buffer;
        uint32_t state;
        uint32_t value;
    };

    struct cmd_bind_index_buffer_request
    {
        object_id command_buffer;
        object_id buffer;
        uint64_t offset;     // VkDeviceSize
        uint32_t index_type; // VkIndexType
        uint32_t reserved;
    };

    struct cmd_draw_indexed_request
    {
        object_id command_buffer;
        uint32_t index_count;
        uint32_t instance_count;
        uint32_t first_index;
        int32_t vertex_offset; // signed, per vkCmdDrawIndexed
        uint32_t first_instance;
        uint32_t reserved;
    };

    struct cmd_end_render_pass_request
    {
        object_id command_buffer;
    };

    // record payload (command::cmd_push_constants) for ioctl_record_commands: in header immediately followed by `size` bytes of data;
    struct cmd_push_constants_request
    {
        object_id command_buffer;
        object_id pipeline_layout;
        uint32_t stage_flags; // VkShaderStageFlags
        uint32_t offset;
        uint32_t size;
        uint32_t reserved;
        // uint8_t values[size];
    };

    // One binding of a descriptor set layout (trailing-array element of create_descriptor_set_layout).
    struct descriptor_set_layout_binding
    {
        uint32_t binding;
        uint32_t descriptor_type;  // VkDescriptorType
        uint32_t descriptor_count; // array size (1 for a scalar binding)
        uint32_t stage_flags;      // VkShaderStageFlags
    };

    // ioctl_create_descriptor_set_layout: in header immediately followed by `binding_count`
    // descriptor_set_layout_binding entries; out = object_response
    struct create_descriptor_set_layout_request
    {
        object_id device;
        uint32_t binding_count;
        uint32_t reserved;
        // descriptor_set_layout_binding bindings[binding_count];
    };

    // One pool size (trailing-array element of create_descriptor_pool).
    struct descriptor_pool_size
    {
        uint32_t descriptor_type; // VkDescriptorType
        uint32_t descriptor_count;
    };

    // ioctl_create_descriptor_pool: in header immediately followed by `pool_size_count`
    // descriptor_pool_size entries; out = object_response
    struct create_descriptor_pool_request
    {
        object_id device;
        uint32_t max_sets;
        uint32_t pool_size_count;
        uint32_t reserved;
        // descriptor_pool_size pool_sizes[pool_size_count];
    };

    // ioctl_allocate_descriptor_sets: in header immediately followed by `set_count` object_id set-layout
    // ids; out = allocate_descriptor_sets_response header followed by `count` object_id set ids
    struct allocate_descriptor_sets_request
    {
        object_id device;
        object_id descriptor_pool;
        uint32_t set_count;
        uint32_t reserved;
        // object_id set_layouts[set_count];
    };

    struct allocate_descriptor_sets_response
    {
        int32_t vk_result;
        uint32_t count;
        // object_id sets[count];
    };

    // One descriptor write (trailing-array element of update_descriptor_sets). Models a single buffer or
    // image descriptor per write (descriptor_count == 1). For buffer types the buffer/offset/range fields
    // apply; for image types (combined image sampler) the sampler/image_view/image_layout fields apply.
    struct descriptor_write
    {
        object_id dst_set;
        uint32_t dst_binding;
        uint32_t dst_array_element;
        uint32_t descriptor_type; // VkDescriptorType
        uint32_t reserved;
        object_id buffer;      // VK_DESCRIPTOR_TYPE_*_BUFFER: the bound buffer (else null_object)
        uint64_t offset;       // buffer offset
        uint64_t range;        // buffer range (VK_WHOLE_SIZE allowed)
        object_id sampler;     // image types: the sampler (else null_object)
        object_id image_view;  // image types: the sampled image view (else null_object)
        uint32_t image_layout; // image types: VkImageLayout
        uint32_t reserved2;
    };

    // ioctl_update_descriptor_sets: in header immediately followed by `write_count` descriptor_write
    // entries; out = result_response
    struct update_descriptor_sets_request
    {
        object_id device;
        uint32_t write_count;
        uint32_t reserved;
        // descriptor_write writes[write_count];
    };

    // immediately followed by `set_count` object_id descriptor-set ids. Bind point is graphics.
    struct cmd_bind_descriptor_sets_request
    {
        object_id command_buffer;
        object_id pipeline_layout;
        uint32_t first_set;
        uint32_t set_count;
        uint32_t bind_point; // VkPipelineBindPoint (0 = graphics, 1 = compute)
        uint32_t reserved;
        // object_id sets[set_count];
    };

    // ioctl_get_surface_capabilities: in (out = raw VkSurfaceCapabilitiesKHR bytes). The bridge has no
    // real surface, so it returns synthetic caps with currentExtent = (0xFFFFFFFF, 0xFFFFFFFF), i.e.
    // "the application chooses the extent". A real driver (when the guest links the real loader) returns
    // the true client-area extent instead.
    struct get_surface_capabilities_request
    {
        object_id physical_device;
        object_id surface;
    };

    // whole command buffer's recording (begin -> cmds -> end) crosses the bridge in a single IOCTL
    // instead of one per command. The buffer is a sequence of records, each a `command_record_header`
    // immediately followed by `size` payload bytes, where the payload is exactly that command's normal
    // request struct (e.g. cmd_draw_request); cmd_push_constants additionally trails its value bytes.
    // `command` is the `command` enum value being recorded. (first non-success
    // VkResult encountered while replaying, or VK_SUCCESS).
    struct command_record_header
    {
        uint32_t command;
        uint32_t size; // payload bytes following this header
    };

    // Portability guard: the guest shim and the emulator host may be built for different bitness (a
    // 32-bit/WOW64 guest talking to a 64-bit host, or vice versa), so every wire struct must have the
    // same layout regardless of pointer size. These structs use only fixed-width integers/floats and
    // `object_id` (a uint64), never `size_t`/pointers, so MSVC lays them out identically on x86 and x64
    // (8-byte types stay 8-aligned on both). The asserts below pin a representative spread of sizes; if
    // anyone introduces a pointer-sized member, the struct's size would differ between architectures and
    // at least one of these would fail to compile.
    static_assert(sizeof(object_id) == 8 && alignof(object_id) == 8, "object_id must be a portable 64-bit value");
    static_assert(sizeof(command_record_header) == 8, "wire layout drift");
    static_assert(sizeof(version_response) == 8, "wire layout drift");
    static_assert(sizeof(allocate_memory_request) == 24, "wire layout drift");
    static_assert(sizeof(bind_buffer_memory_request) == 32, "wire layout drift");
    static_assert(sizeof(cmd_draw_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_bind_pipeline_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_dispatch_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_dispatch_indirect_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_clear_depth_stencil_image_request) == 56, "wire layout drift");
    static_assert(sizeof(get_image_subresource_layout_request) == 32, "wire layout drift");
    static_assert(sizeof(get_image_subresource_layout_response) == 48, "wire layout drift");
    static_assert(sizeof(create_buffer_view_request) == 40, "wire layout drift");
    static_assert(sizeof(buffer_copy_region) == 24, "wire layout drift");
    static_assert(sizeof(cmd_copy_buffer_request) == 32, "wire layout drift");
    static_assert(sizeof(create_query_pool_request) == 24, "wire layout drift");
    static_assert(sizeof(get_query_pool_results_request) == 40, "wire layout drift");
    static_assert(sizeof(get_query_pool_results_response) == 8, "wire layout drift");
    static_assert(sizeof(cmd_reset_query_pool_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_begin_query_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_end_query_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_write_timestamp_request) == 24, "wire layout drift");
    static_assert(sizeof(reset_query_pool_request) == 24, "wire layout drift");
    static_assert(sizeof(vertex_buffer_binding) == 16, "wire layout drift");
    static_assert(sizeof(vertex_buffer_binding2) == 32, "wire layout drift");
    static_assert(sizeof(cmd_bind_vertex_buffers2_request) == 24, "wire layout drift");
    static_assert(sizeof(rendering_attachment) == 56, "wire layout drift");
    static_assert(sizeof(cmd_begin_rendering_request) == 48, "wire layout drift");
    static_assert(sizeof(cmd_end_rendering_request) == 8, "wire layout drift");
    static_assert(sizeof(allocate_command_buffer_request) == 24, "wire layout drift");
    static_assert(sizeof(begin_command_buffer_request) == 40, "wire layout drift");
    static_assert(sizeof(cmd_execute_commands_request) == 16, "wire layout drift");
    static_assert(sizeof(viewport_entry) == 24, "wire layout drift");
    static_assert(sizeof(cmd_set_viewport_request) == 24, "wire layout drift");
    static_assert(sizeof(scissor_entry) == 16, "wire layout drift");
    static_assert(sizeof(cmd_set_scissor_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_set_depth_bias_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_set_blend_constants_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_set_depth_bounds_request) == 16, "wire layout drift");
    static_assert(sizeof(cmd_set_line_width_request) == 16, "wire layout drift");
    static_assert(sizeof(cmd_set_stencil_request) == 24, "wire layout drift");
    static_assert(sizeof(cmd_set_stencil_op_request) == 32, "wire layout drift");
    static_assert(sizeof(cmd_set_dynamic_u32_request) == 16, "wire layout drift");
    static_assert(sizeof(descriptor_set_layout_binding) == 16, "wire layout drift");
    static_assert(sizeof(cmd_bind_descriptor_sets_request) == 32, "wire layout drift");
    static_assert(sizeof(create_sampler_request) == 32, "wire layout drift");
    static_assert(sizeof(enumerate_device_extension_properties_request) == 16, "wire layout drift");
    static_assert(sizeof(enumerate_device_extension_properties_response) == 8, "wire layout drift");
    static_assert(sizeof(feature_chain_record) == 8, "wire layout drift");
    static_assert(sizeof(get_physical_device_features2_request) == 16, "wire layout drift");
    static_assert(sizeof(get_physical_device_features2_response) == 8, "wire layout drift");
    static_assert(sizeof(get_physical_device_properties2_request) == 16, "wire layout drift");
    static_assert(sizeof(get_physical_device_properties2_response) == 8, "wire layout drift");
    static_assert(sizeof(create_device_request) == 32, "wire layout drift");
    static_assert(sizeof(device_queue_create_entry) == 8, "wire layout drift");
    static_assert(sizeof(queue_wait_idle_request) == 8, "wire layout drift");
    static_assert(sizeof(device_wait_idle_request) == 8, "wire layout drift");
    static_assert(sizeof(reset_command_pool_request) == 24, "wire layout drift");
    static_assert(sizeof(reset_command_buffer_request) == 16, "wire layout drift");
    static_assert(sizeof(get_physical_device_image_format_properties_request) == 32, "wire layout drift");
    static_assert(sizeof(get_physical_device_image_format_properties_response) == 40, "wire layout drift");
}
