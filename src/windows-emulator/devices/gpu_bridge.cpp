#include "../std_include.hpp"
#include "gpu_bridge.hpp"
#include "vulkan_host.hpp"
#include "../windows_emulator.hpp"

#include <gpu_bridge_protocol.hpp>

namespace sogen
{
    namespace
    {
        // Host endpoint of the GPU paravirtualization bridge. The guest reaches it by opening
        // \\.\SogenGpu and issuing IOCTLs; each control code maps to one bridge command, with the
        // payload carried in the input/output buffers. Real Vulkan objects live on the host (behind
        // vulkan_host) and the guest only ever sees opaque object ids.
        //
        // Live Vulkan state cannot be serialized, so this device intentionally does not participate
        // in snapshots yet; restoring with an open GPU handle is an experimental limitation.
        struct gpu_bridge_device : io_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                switch (context.io_control_code)
                {
                case gpu_bridge::ioctl_get_version:
                    return handle_get_version(win_emu, context);
                case gpu_bridge::ioctl_create_instance:
                    return handle_create_instance(win_emu, context);
                case gpu_bridge::ioctl_destroy_instance:
                    return handle_destroy_instance(win_emu, context);
                case gpu_bridge::ioctl_enumerate_physical_devices:
                    return handle_enumerate_physical_devices(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_properties:
                    return handle_get_physical_device_properties(win_emu, context);
                case gpu_bridge::ioctl_get_queue_family_properties:
                    return handle_get_queue_family_properties(win_emu, context);
                case gpu_bridge::ioctl_enumerate_device_extension_properties:
                    return handle_enumerate_device_extension_properties(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_features2:
                    return handle_get_physical_device_features2(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_properties2:
                    return handle_get_physical_device_properties2(win_emu, context);
                case gpu_bridge::ioctl_create_semaphore:
                    return handle_create_semaphore(win_emu, context);
                case gpu_bridge::ioctl_destroy_semaphore:
                    return handle_destroy_semaphore(win_emu, context);
                case gpu_bridge::ioctl_get_semaphore_counter_value:
                    return handle_get_semaphore_counter_value(win_emu, context);
                case gpu_bridge::ioctl_signal_semaphore:
                    return handle_signal_semaphore(win_emu, context);
                case gpu_bridge::ioctl_wait_semaphores:
                    return handle_wait_semaphores(win_emu, context);
                case gpu_bridge::ioctl_get_buffer_device_address:
                    return handle_get_buffer_device_address(win_emu, context);
                case gpu_bridge::ioctl_queue_submit2:
                    return handle_queue_submit2(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_format_properties:
                    return handle_get_physical_device_format_properties(win_emu, context);
                case gpu_bridge::ioctl_create_compute_pipeline:
                    return handle_create_compute_pipeline(win_emu, context);
                case gpu_bridge::ioctl_create_device:
                    return handle_create_device(win_emu, context);
                case gpu_bridge::ioctl_get_device_queue:
                    return handle_get_device_queue(win_emu, context);
                case gpu_bridge::ioctl_destroy_device:
                    return handle_destroy_device(win_emu, context);
                case gpu_bridge::ioctl_create_command_pool:
                    return handle_create_command_pool(win_emu, context);
                case gpu_bridge::ioctl_destroy_command_pool:
                    return handle_destroy_command_pool(win_emu, context);
                case gpu_bridge::ioctl_allocate_command_buffer:
                    return handle_allocate_command_buffer(win_emu, context);
                case gpu_bridge::ioctl_free_command_buffer:
                    return handle_free_command_buffer(win_emu, context);
                case gpu_bridge::ioctl_create_fence:
                    return handle_create_fence(win_emu, context);
                case gpu_bridge::ioctl_destroy_fence:
                    return handle_destroy_fence(win_emu, context);
                case gpu_bridge::ioctl_reset_fence:
                    return handle_reset_fence(win_emu, context);
                case gpu_bridge::ioctl_get_fence_status:
                    return handle_get_fence_status(win_emu, context);
                case gpu_bridge::ioctl_queue_submit:
                    return handle_queue_submit(win_emu, context);
                case gpu_bridge::ioctl_queue_wait_idle:
                    return handle_queue_wait_idle(win_emu, context);
                case gpu_bridge::ioctl_device_wait_idle:
                    return handle_device_wait_idle(win_emu, context);
                case gpu_bridge::ioctl_reset_command_pool:
                    return handle_reset_command_pool(win_emu, context);
                case gpu_bridge::ioctl_reset_command_buffer:
                    return handle_reset_command_buffer(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_memory_properties:
                    return handle_get_physical_device_memory_properties(win_emu, context);
                case gpu_bridge::ioctl_allocate_memory:
                    return handle_allocate_memory(win_emu, context);
                case gpu_bridge::ioctl_free_memory:
                    return handle_free_memory(win_emu, context);
                case gpu_bridge::ioctl_create_buffer:
                    return handle_create_buffer(win_emu, context);
                case gpu_bridge::ioctl_destroy_buffer:
                    return handle_destroy_buffer(win_emu, context);
                case gpu_bridge::ioctl_get_buffer_memory_requirements:
                    return handle_get_buffer_memory_requirements(win_emu, context);
                case gpu_bridge::ioctl_bind_buffer_memory:
                    return handle_bind_buffer_memory(win_emu, context);
                case gpu_bridge::ioctl_download_memory:
                    return handle_download_memory(win_emu, context);
                case gpu_bridge::ioctl_upload_memory:
                    return handle_upload_memory(win_emu, context);
                case gpu_bridge::ioctl_create_image:
                    return handle_create_image(win_emu, context);
                case gpu_bridge::ioctl_get_physical_device_image_format_properties:
                    return handle_get_physical_device_image_format_properties(win_emu, context);
                case gpu_bridge::ioctl_destroy_image:
                    return handle_destroy_image(win_emu, context);
                case gpu_bridge::ioctl_get_image_memory_requirements:
                    return handle_get_image_memory_requirements(win_emu, context);
                case gpu_bridge::ioctl_get_image_subresource_layout:
                    return handle_get_image_subresource_layout(win_emu, context);
                case gpu_bridge::ioctl_bind_image_memory:
                    return handle_bind_image_memory(win_emu, context);
                case gpu_bridge::ioctl_create_surface:
                    return handle_create_surface(win_emu, context);
                case gpu_bridge::ioctl_destroy_surface:
                    return handle_destroy_surface(win_emu, context);
                case gpu_bridge::ioctl_create_swapchain:
                    return handle_create_swapchain(win_emu, context);
                case gpu_bridge::ioctl_destroy_swapchain:
                    return handle_destroy_swapchain(win_emu, context);
                case gpu_bridge::ioctl_get_swapchain_images:
                    return handle_get_swapchain_images(win_emu, context);
                case gpu_bridge::ioctl_acquire_next_image:
                    return handle_acquire_next_image(win_emu, context);
                case gpu_bridge::ioctl_queue_present:
                    return handle_queue_present(win_emu, context);
                case gpu_bridge::ioctl_create_shader_module:
                    return handle_create_shader_module(win_emu, context);
                case gpu_bridge::ioctl_destroy_shader_module:
                    return handle_destroy_shader_module(win_emu, context);
                case gpu_bridge::ioctl_create_image_view:
                    return handle_create_image_view(win_emu, context);
                case gpu_bridge::ioctl_destroy_image_view:
                    return handle_destroy_image_view(win_emu, context);
                case gpu_bridge::ioctl_create_buffer_view:
                    return handle_create_buffer_view(win_emu, context);
                case gpu_bridge::ioctl_destroy_buffer_view:
                    return handle_destroy_buffer_view(win_emu, context);
                case gpu_bridge::ioctl_create_query_pool:
                    return handle_create_query_pool(win_emu, context);
                case gpu_bridge::ioctl_destroy_query_pool:
                    return handle_destroy_query_pool(win_emu, context);
                case gpu_bridge::ioctl_get_query_pool_results:
                    return handle_get_query_pool_results(win_emu, context);
                case gpu_bridge::ioctl_reset_query_pool:
                    return handle_reset_query_pool(win_emu, context);
                case gpu_bridge::ioctl_create_render_pass:
                    return handle_create_render_pass(win_emu, context);
                case gpu_bridge::ioctl_destroy_render_pass:
                    return handle_destroy_render_pass(win_emu, context);
                case gpu_bridge::ioctl_create_framebuffer:
                    return handle_create_framebuffer(win_emu, context);
                case gpu_bridge::ioctl_destroy_framebuffer:
                    return handle_destroy_framebuffer(win_emu, context);
                case gpu_bridge::ioctl_create_pipeline_layout:
                    return handle_create_pipeline_layout(win_emu, context);
                case gpu_bridge::ioctl_destroy_pipeline_layout:
                    return handle_destroy_pipeline_layout(win_emu, context);
                case gpu_bridge::ioctl_create_graphics_pipeline:
                    return handle_create_graphics_pipeline(win_emu, context);
                case gpu_bridge::ioctl_destroy_pipeline:
                    return handle_destroy_pipeline(win_emu, context);
                case gpu_bridge::ioctl_get_surface_capabilities:
                    return handle_get_surface_capabilities(win_emu, context);
                case gpu_bridge::ioctl_record_commands:
                    return handle_record_commands(win_emu, context);
                case gpu_bridge::ioctl_create_descriptor_set_layout:
                    return handle_create_descriptor_set_layout(win_emu, context);
                case gpu_bridge::ioctl_destroy_descriptor_set_layout:
                    return handle_destroy_descriptor_set_layout(win_emu, context);
                case gpu_bridge::ioctl_create_descriptor_pool:
                    return handle_create_descriptor_pool(win_emu, context);
                case gpu_bridge::ioctl_destroy_descriptor_pool:
                    return handle_destroy_descriptor_pool(win_emu, context);
                case gpu_bridge::ioctl_allocate_descriptor_sets:
                    return handle_allocate_descriptor_sets(win_emu, context);
                case gpu_bridge::ioctl_update_descriptor_sets:
                    return handle_update_descriptor_sets(win_emu, context);
                case gpu_bridge::ioctl_create_sampler:
                    return handle_create_sampler(win_emu, context);
                case gpu_bridge::ioctl_destroy_sampler:
                    return handle_destroy_sampler(win_emu, context);

                default:
                    win_emu.log.warn("[gpu-bridge] Unsupported IOCTL: 0x%X\n", static_cast<unsigned>(context.io_control_code));
                    return STATUS_NOT_SUPPORTED;
                }
            }

            void serialize_object(utils::buffer_serializer&) const override
            {
            }

            void deserialize_object(utils::buffer_deserializer&) override
            {
            }

          private:
            vulkan_host vulkan_{};

            static void set_information(const io_device_context& context, const ULONG bytes)
            {
                if (context.io_status_block)
                {
                    context.io_status_block.access([&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = bytes; });
                }
            }

            // Reads a fixed-size request struct from the guest input buffer.
            template <typename Request>
            static bool read_input(windows_emulator& win_emu, const io_device_context& context, Request& out)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(Request))
                {
                    return false;
                }
                out = emulator_object<Request>{win_emu.emu(), context.input_buffer}.read();
                return true;
            }

            // Reads `count` trailing elements of type T from the input buffer starting at `offset` (just
            // past a fixed header), bounded by the buffer length. Returns false on a malformed/too-short
            // buffer.
            template <typename T>
            static bool read_trailing_array(windows_emulator& win_emu, const io_device_context& context, size_t offset, uint32_t count,
                                            std::vector<T>& out)
            {
                out.clear();
                if (count == 0)
                {
                    return true;
                }
                // Validate the buffer holds the bytes before allocating, so a malformed request with a
                // huge count can't force a large allocation. Subtraction avoids offset+bytes overflow.
                const size_t bytes = static_cast<size_t>(count) * sizeof(T);
                if (!context.input_buffer || context.input_buffer_length < offset || context.input_buffer_length - offset < bytes)
                {
                    return false;
                }
                out.assign(count, T{});
                win_emu.emu().read_memory(context.input_buffer + offset, out.data(), bytes);
                return true;
            }

            // Writes a fixed-size response struct to the guest output buffer and records its size.
            template <typename Response>
            static NTSTATUS write_output(windows_emulator& win_emu, const io_device_context& context, const Response& response)
            {
                if (!context.output_buffer || context.output_buffer_length < sizeof(Response))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }
                emulator_object<Response>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, static_cast<ULONG>(sizeof(Response)));
                return STATUS_SUCCESS;
            }

            static NTSTATUS handle_get_version(windows_emulator& win_emu, const io_device_context& context)
            {
                constexpr auto response_size = static_cast<ULONG>(sizeof(gpu_bridge::version_response));

                if (!context.output_buffer || context.output_buffer_length < response_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                constexpr gpu_bridge::version_response response{
                    .magic = gpu_bridge::protocol_magic,
                    .version = gpu_bridge::protocol_version,
                };

                emulator_object<gpu_bridge::version_response>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, response_size);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_instance(windows_emulator& win_emu, const io_device_context& context)
            {
                constexpr auto response_size = static_cast<ULONG>(sizeof(gpu_bridge::create_instance_response));

                if (!context.output_buffer || context.output_buffer_length < response_size)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                uint64_t instance = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_instance(instance);

                const gpu_bridge::create_instance_response response{
                    .vk_result = result,
                    .reserved = 0,
                    .instance = instance,
                };

                emulator_object<gpu_bridge::create_instance_response>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, response_size);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_destroy_instance(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(gpu_bridge::destroy_instance_request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto request = emulator_object<gpu_bridge::destroy_instance_request>{win_emu.emu(), context.input_buffer}.read();

                this->vulkan_.destroy_instance(request.instance);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_enumerate_physical_devices(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::enumerate_physical_devices_request;
                using response_t = gpu_bridge::enumerate_physical_devices_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                const auto array_capacity =
                    static_cast<uint32_t>((context.output_buffer_length - sizeof(response_t)) / sizeof(gpu_bridge::object_id));
                const auto max_count = std::min(request.max_count, array_capacity);

                std::vector<uint64_t> ids(max_count);
                uint32_t count = 0;
                const int32_t result = this->vulkan_.enumerate_physical_devices(request.instance, std::span{ids}, count);

                const response_t response{
                    .vk_result = result,
                    .count = count,
                };
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);

                const auto written = std::min(count, max_count);
                if (written > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), ids.data(),
                                               written * sizeof(gpu_bridge::object_id));
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + written * sizeof(gpu_bridge::object_id)));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_physical_device_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_physical_device_properties_request;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length == 0)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                std::vector<std::byte> properties(context.output_buffer_length);
                const int32_t result = this->vulkan_.get_physical_device_properties(request.physical_device, properties.data(),
                                                                                    properties.size(), win_emu.process.is_wow64_process);
                if (result != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                win_emu.emu().write_memory(context.output_buffer, properties.data(), properties.size());
                set_information(context, context.output_buffer_length);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_queue_family_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_queue_family_properties_request;
                using response_t = gpu_bridge::get_queue_family_properties_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();
                const auto array_bytes = context.output_buffer_length - static_cast<uint32_t>(sizeof(response_t));

                std::vector<std::byte> properties(array_bytes);
                uint32_t count = 0;
                const int32_t result =
                    this->vulkan_.get_queue_family_properties(request.physical_device, properties.data(), properties.size(), count);
                if (result != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const response_t response{.count = count, .reserved = 0};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);

                if (array_bytes > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), properties.data(), array_bytes);
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + array_bytes));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_enumerate_device_extension_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::enumerate_device_extension_properties_request;
                using response_t = gpu_bridge::enumerate_device_extension_properties_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();
                const auto array_bytes = context.output_buffer_length - static_cast<uint32_t>(sizeof(response_t));

                std::vector<std::byte> properties(array_bytes);
                uint32_t count = 0;
                const int32_t result = this->vulkan_.enumerate_device_extension_properties(request.physical_device, properties.data(),
                                                                                           properties.size(), count);

                const response_t response{.vk_result = result, .count = count};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);

                if (array_bytes > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), properties.data(), array_bytes);
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + array_bytes));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_physical_device_features2(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_physical_device_features2_request;
                using response_t = gpu_bridge::get_physical_device_features2_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                const auto records_bytes = context.input_buffer_length - static_cast<uint32_t>(sizeof(request_t));
                std::vector<std::byte> records(records_bytes);
                if (records_bytes > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), records.data(), records_bytes);
                }

                std::vector<std::byte> blob;
                const int32_t result = this->vulkan_.get_physical_device_features2(request.physical_device, records.data(), records.size(),
                                                                                   request.struct_count, blob);

                const response_t response{.vk_result = result, .struct_count = request.struct_count};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);

                const auto avail = context.output_buffer_length - static_cast<uint32_t>(sizeof(response_t));
                const auto to_write = static_cast<uint32_t>(blob.size() < avail ? blob.size() : avail);
                if (to_write > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), blob.data(), to_write);
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + to_write));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_physical_device_properties2(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_physical_device_properties2_request;
                using response_t = gpu_bridge::get_physical_device_properties2_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                const auto records_bytes = context.input_buffer_length - static_cast<uint32_t>(sizeof(request_t));
                std::vector<std::byte> records(records_bytes);
                if (records_bytes > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), records.data(), records_bytes);
                }

                std::vector<std::byte> blob;
                const int32_t result = this->vulkan_.get_physical_device_properties2(request.physical_device, records.data(),
                                                                                     records.size(), request.struct_count, blob);

                const response_t response{.vk_result = result, .struct_count = request.struct_count};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);

                const auto avail = context.output_buffer_length - static_cast<uint32_t>(sizeof(response_t));
                const auto to_write = static_cast<uint32_t>(blob.size() < avail ? blob.size() : avail);
                if (to_write > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), blob.data(), to_write);
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + to_write));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_device(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_device_request;
                using response_t = gpu_bridge::create_device_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                // Read the trailing [extension names][feature records] blobs.
                const auto trailing = context.input_buffer_length - static_cast<uint32_t>(sizeof(request_t));
                std::vector<std::byte> blob(trailing);
                if (trailing > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), blob.data(), trailing);
                }

                const std::byte* extension_blob = nullptr;
                const std::byte* feature_blob = nullptr;
                uint32_t extension_count = 0;
                uint32_t feature_struct_count = 0;
                size_t extension_blob_size = 0;
                size_t feature_blob_size = 0;
                if (static_cast<size_t>(request.extension_blob_size) + request.feature_blob_size <= blob.size())
                {
                    extension_blob = blob.data();
                    extension_blob_size = request.extension_blob_size;
                    extension_count = request.extension_count;
                    feature_blob = blob.data() + request.extension_blob_size;
                    feature_blob_size = request.feature_blob_size;
                    feature_struct_count = request.feature_struct_count;
                }

                uint64_t device = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_device(request.physical_device, request.queue_family_index, request.queue_count,
                                                                   extension_blob, extension_blob_size, extension_count, feature_blob,
                                                                   feature_blob_size, feature_struct_count, device);

                const response_t response{
                    .vk_result = result,
                    .reserved = 0,
                    .device = device,
                };
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, static_cast<ULONG>(sizeof(response_t)));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_device_queue(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_device_queue_request;
                using response_t = gpu_bridge::get_device_queue_response;

                if (!context.input_buffer || context.input_buffer_length < sizeof(request_t))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto request = emulator_object<request_t>{win_emu.emu(), context.input_buffer}.read();

                uint64_t queue = gpu_bridge::null_object;
                this->vulkan_.get_device_queue(request.device, request.queue_family_index, request.queue_index, queue);

                const response_t response{.queue = queue};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);
                set_information(context, static_cast<ULONG>(sizeof(response_t)));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_destroy_device(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(gpu_bridge::destroy_device_request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto request = emulator_object<gpu_bridge::destroy_device_request>{win_emu.emu(), context.input_buffer}.read();

                this->vulkan_.destroy_device(request.device);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_command_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_command_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t pool = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_command_pool(request.device, request.queue_family_index, request.flags, pool);
                return write_output(win_emu, context,
                                    gpu_bridge::create_command_pool_response{.vk_result = result, .reserved = 0, .command_pool = pool});
            }

            NTSTATUS handle_destroy_command_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_command_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_command_pool(request.device, request.command_pool);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_allocate_command_buffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::allocate_command_buffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t command_buffer = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.allocate_command_buffer(request.device, request.command_pool, command_buffer);
                return write_output(
                    win_emu, context,
                    gpu_bridge::allocate_command_buffer_response{.vk_result = result, .reserved = 0, .command_buffer = command_buffer});
            }

            NTSTATUS handle_free_command_buffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::free_command_buffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.free_command_buffer(request.device, request.command_pool, request.command_buffer);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_fence(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_fence_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t fence = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_fence(request.device, request.flags, fence);
                return write_output(win_emu, context,
                                    gpu_bridge::create_fence_response{.vk_result = result, .reserved = 0, .fence = fence});
            }

            NTSTATUS handle_destroy_fence(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_fence_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_fence(request.device, request.fence);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_semaphore(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_semaphore_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t semaphore = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_semaphore(request.device, request.flags, request.semaphore_type, request.initial_value, semaphore);
                return write_output(win_emu, context,
                                    gpu_bridge::create_semaphore_response{.vk_result = result, .reserved = 0, .semaphore = semaphore});
            }

            NTSTATUS handle_destroy_semaphore(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_semaphore_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_semaphore(request.device, request.semaphore);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_semaphore_counter_value(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_semaphore_counter_value_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t value = 0;
                const int32_t result = this->vulkan_.get_semaphore_counter_value(request.device, request.semaphore, value);
                return write_output(win_emu, context,
                                    gpu_bridge::get_semaphore_counter_value_response{.vk_result = result, .reserved = 0, .value = value});
            }

            NTSTATUS handle_signal_semaphore(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::signal_semaphore_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.signal_semaphore(request.device, request.semaphore, request.value);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_wait_semaphores(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::wait_semaphores_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<gpu_bridge::wait_semaphore_entry> entries;
                if (!read_trailing_array(win_emu, context, sizeof(request), request.semaphore_count, entries))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.wait_semaphores(request.device, request.flags, entries.data(), request.semaphore_count, request.timeout);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_get_buffer_device_address(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_buffer_device_address_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const uint64_t address = this->vulkan_.get_buffer_device_address(request.device, request.buffer);
                return write_output(win_emu, context, gpu_bridge::get_buffer_device_address_response{.address = address});
            }

            NTSTATUS handle_get_physical_device_format_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_physical_device_format_properties_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint32_t linear = 0;
                uint32_t optimal = 0;
                uint32_t buffer = 0;
                this->vulkan_.get_physical_device_format_properties(request.physical_device, request.format, linear, optimal, buffer);
                return write_output(
                    win_emu, context,
                    gpu_bridge::get_physical_device_format_properties_response{
                        .linear_tiling_features = linear, .optimal_tiling_features = optimal, .buffer_features = buffer, .reserved = 0});
            }

            NTSTATUS handle_queue_submit2(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::queue_submit2_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const size_t wait_offset = sizeof(request);
                const size_t cmd_offset =
                    wait_offset + static_cast<size_t>(request.wait_count) * sizeof(gpu_bridge::submit2_semaphore_entry);
                const size_t signal_offset = cmd_offset + static_cast<size_t>(request.command_buffer_count) * sizeof(uint64_t);

                std::vector<gpu_bridge::submit2_semaphore_entry> waits;
                std::vector<uint64_t> command_buffers;
                std::vector<gpu_bridge::submit2_semaphore_entry> signals;
                if (!read_trailing_array(win_emu, context, wait_offset, request.wait_count, waits) ||
                    !read_trailing_array(win_emu, context, cmd_offset, request.command_buffer_count, command_buffers) ||
                    !read_trailing_array(win_emu, context, signal_offset, request.signal_count, signals))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.queue_submit2(request.queue, request.fence, waits.data(), request.wait_count, command_buffers.data(),
                                                request.command_buffer_count, signals.data(), request.signal_count);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_create_compute_pipeline(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_compute_pipeline_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t pipeline = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_compute_pipeline(request.device, request.pipeline_layout, request.shader_module, pipeline);
                return write_output(win_emu, context,
                                    gpu_bridge::create_compute_pipeline_response{.vk_result = result, .reserved = 0, .pipeline = pipeline});
            }

            NTSTATUS handle_reset_fence(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::reset_fence_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.reset_fence(request.device, request.fence);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_get_fence_status(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_fence_status_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.get_fence_status(request.fence);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_queue_submit(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::queue_submit_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.queue_submit(request.queue, request.command_buffer, request.fence);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_queue_wait_idle(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::queue_wait_idle_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.queue_wait_idle(request.queue);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_device_wait_idle(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_wait_idle_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.device_wait_idle(request.device);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_reset_command_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::reset_command_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.reset_command_pool(request.device, request.command_pool, request.flags);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_reset_command_buffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::reset_command_buffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.reset_command_buffer(request.command_buffer, request.flags);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_get_physical_device_memory_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_physical_device_memory_properties_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length == 0)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::vector<std::byte> properties(context.output_buffer_length);
                const int32_t result =
                    this->vulkan_.get_physical_device_memory_properties(request.physical_device, properties.data(), properties.size());
                if (result != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                win_emu.emu().write_memory(context.output_buffer, properties.data(), properties.size());
                set_information(context, context.output_buffer_length);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_allocate_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::allocate_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t memory = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.allocate_memory(request.device, request.size, request.memory_type_index, memory);
                return write_output(win_emu, context,
                                    gpu_bridge::allocate_memory_response{.vk_result = result, .reserved = 0, .memory = memory});
            }

            NTSTATUS handle_free_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::free_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.free_memory(request.device, request.memory);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_buffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_buffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t buffer = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_buffer(request.device, request.size, request.usage, buffer);
                return write_output(win_emu, context,
                                    gpu_bridge::create_buffer_response{.vk_result = result, .reserved = 0, .buffer = buffer});
            }

            NTSTATUS handle_destroy_buffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_buffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_buffer(request.device, request.buffer);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_buffer_memory_requirements(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_buffer_memory_requirements_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t size = 0;
                uint64_t alignment = 0;
                uint32_t memory_type_bits = 0;
                const int32_t result =
                    this->vulkan_.get_buffer_memory_requirements(request.device, request.buffer, size, alignment, memory_type_bits);
                return write_output(win_emu, context,
                                    gpu_bridge::memory_requirements_response{
                                        .vk_result = result, .memory_type_bits = memory_type_bits, .size = size, .alignment = alignment});
            }

            NTSTATUS handle_bind_buffer_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::bind_buffer_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.bind_buffer_memory(request.device, request.buffer, request.memory, request.offset);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_download_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::download_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length == 0)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto copy_bytes = std::min<uint64_t>(request.size, context.output_buffer_length);

                std::vector<std::byte> bytes(static_cast<size_t>(copy_bytes));
                const int32_t result =
                    this->vulkan_.download_memory(request.device, request.memory, request.offset, copy_bytes, bytes.data(), bytes.size());
                if (result != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (copy_bytes > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer, bytes.data(), bytes.size());
                }
                set_information(context, static_cast<ULONG>(copy_bytes));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_upload_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::upload_memory_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto payload = std::min<uint64_t>(request.size, context.input_buffer_length - sizeof(request_t));

                std::vector<std::byte> bytes(static_cast<size_t>(payload));
                if (payload > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), bytes.data(), bytes.size());
                }

                const int32_t result =
                    this->vulkan_.upload_memory(request.device, request.memory, request.offset, payload, bytes.data(), bytes.size());
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            static vulkan_host::subresource_range to_host_range(const gpu_bridge::image_subresource_range& range)
            {
                return vulkan_host::subresource_range{
                    .aspect_mask = range.aspect_mask,
                    .base_mip_level = range.base_mip_level,
                    .level_count = range.level_count,
                    .base_array_layer = range.base_array_layer,
                    .layer_count = range.layer_count,
                };
            }

            NTSTATUS handle_create_image(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_image_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t image = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_image(request.device, request.format, request.width, request.height,
                                                                  request.usage, request.tiling, request.samples, image);
                return write_output(win_emu, context,
                                    gpu_bridge::create_image_response{.vk_result = result, .reserved = 0, .image = image});
            }

            NTSTATUS handle_get_physical_device_image_format_properties(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_physical_device_image_format_properties_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                vulkan_host::image_format_properties props{};
                const int32_t result = this->vulkan_.get_physical_device_image_format_properties(
                    request.physical_device, request.format, request.type, request.tiling, request.usage, request.flags, props);
                return write_output(
                    win_emu, context,
                    gpu_bridge::get_physical_device_image_format_properties_response{.vk_result = result,
                                                                                     .max_mip_levels = props.max_mip_levels,
                                                                                     .max_array_layers = props.max_array_layers,
                                                                                     .sample_counts = props.sample_counts,
                                                                                     .max_extent_width = props.max_extent_width,
                                                                                     .max_extent_height = props.max_extent_height,
                                                                                     .max_extent_depth = props.max_extent_depth,
                                                                                     .reserved = 0,
                                                                                     .max_resource_size = props.max_resource_size});
            }

            NTSTATUS handle_destroy_image(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_image_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_image(request.device, request.image);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_image_memory_requirements(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_image_memory_requirements_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t size = 0;
                uint64_t alignment = 0;
                uint32_t memory_type_bits = 0;
                const int32_t result =
                    this->vulkan_.get_image_memory_requirements(request.device, request.image, size, alignment, memory_type_bits);
                return write_output(win_emu, context,
                                    gpu_bridge::memory_requirements_response{
                                        .vk_result = result, .memory_type_bits = memory_type_bits, .size = size, .alignment = alignment});
            }

            NTSTATUS handle_get_image_subresource_layout(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_image_subresource_layout_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t offset = 0;
                uint64_t size = 0;
                uint64_t row_pitch = 0;
                uint64_t array_pitch = 0;
                uint64_t depth_pitch = 0;
                const int32_t result =
                    this->vulkan_.get_image_subresource_layout(request.device, request.image, request.aspect_mask, request.mip_level,
                                                               request.array_layer, offset, size, row_pitch, array_pitch, depth_pitch);
                return write_output(win_emu, context,
                                    gpu_bridge::get_image_subresource_layout_response{.vk_result = result,
                                                                                      .reserved = 0,
                                                                                      .offset = offset,
                                                                                      .size = size,
                                                                                      .row_pitch = row_pitch,
                                                                                      .array_pitch = array_pitch,
                                                                                      .depth_pitch = depth_pitch});
            }

            NTSTATUS handle_bind_image_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::bind_image_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.bind_image_memory(request.device, request.image, request.memory, request.offset);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_create_surface(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_surface_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t surface = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_surface(request.hwnd, surface);
                return write_output(win_emu, context,
                                    gpu_bridge::create_surface_response{.vk_result = result, .reserved = 0, .surface = surface});
            }

            NTSTATUS handle_destroy_surface(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_surface_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_surface(request.surface);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_swapchain(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_swapchain_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t swapchain = gpu_bridge::null_object;
                uint32_t image_count = 0;
                const int32_t result =
                    this->vulkan_.create_swapchain(request.device, request.surface, request.format, request.width, request.height,
                                                   request.min_image_count, request.image_usage, swapchain, image_count);
                return write_output(
                    win_emu, context,
                    gpu_bridge::create_swapchain_response{.vk_result = result, .image_count = image_count, .swapchain = swapchain});
            }

            NTSTATUS handle_destroy_swapchain(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_swapchain_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_swapchain(request.device, request.swapchain);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_swapchain_images(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_swapchain_images_request;
                using response_t = gpu_bridge::get_swapchain_images_response;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto array_capacity =
                    static_cast<uint32_t>((context.output_buffer_length - sizeof(response_t)) / sizeof(gpu_bridge::object_id));
                const auto max_count = std::min(request.max_count, array_capacity);

                std::vector<uint64_t> ids(max_count);
                uint32_t count = 0;
                const int32_t result = this->vulkan_.get_swapchain_images(request.swapchain, std::span{ids}, count);

                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response_t{.vk_result = result, .count = count});

                const auto written = std::min(count, max_count);
                if (written > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), ids.data(),
                                               written * sizeof(gpu_bridge::object_id));
                }

                set_information(context, static_cast<ULONG>(sizeof(response_t) + written * sizeof(gpu_bridge::object_id)));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_acquire_next_image(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::acquire_next_image_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint32_t index = 0;
                const int32_t result = this->vulkan_.acquire_next_image(request.swapchain, index);
                return write_output(win_emu, context, gpu_bridge::acquire_next_image_response{.vk_result = result, .image_index = index});
            }

            NTSTATUS handle_queue_present(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::queue_present_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> pixels;
                uint32_t width = 0;
                uint32_t height = 0;
                uint64_t hwnd_value = 0;
                const int32_t result =
                    this->vulkan_.queue_present(request.queue, request.swapchain, request.image_index, pixels, width, height, hwnd_value);

                // Hand the freshly read-back pixels to the guest window through the UI backend (the same
                // seam GDI EndPaint uses). The swapchain is B8G8R8A8, matching bgra8, so no swizzle.
                if (result == 0 /* VK_SUCCESS */ && hwnd_value != 0 && !pixels.empty())
                {
                    win_emu.ui().present_surface(static_cast<hwnd>(hwnd_value), ui_surface_desc{
                                                                                    .width = static_cast<int>(width),
                                                                                    .height = static_cast<int>(height),
                                                                                    .stride = static_cast<int>(width * 4),
                                                                                    .format = ui_surface_format::bgra8,
                                                                                    .pixels = pixels.data(),
                                                                                });
                }

                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_create_shader_module(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_shader_module_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto code_bytes = std::min<uint64_t>(request.code_size, context.input_buffer_length - sizeof(request_t));
                std::vector<std::byte> code(static_cast<size_t>(code_bytes));
                if (code_bytes > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), code.data(), code.size());
                }

                uint64_t module = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_shader_module(request.device, code.data(), code.size(), module);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = module});
            }

            NTSTATUS handle_destroy_shader_module(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_shader_module(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_image_view(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_image_view_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t view = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_image_view(request.device, request.image, request.format, request.aspect_mask, view);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = view});
            }

            NTSTATUS handle_destroy_image_view(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_image_view(request.device, request.object);
                return STATUS_SUCCESS;
            }
            NTSTATUS handle_create_buffer_view(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_buffer_view_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t view = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_buffer_view(request.device, request.buffer, request.format, request.offset, request.range, view);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = view});
            }
            NTSTATUS handle_destroy_buffer_view(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_buffer_view(request.device, request.object);
                return STATUS_SUCCESS;
            }
            NTSTATUS handle_create_query_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_query_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t pool = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_query_pool(request.device, request.query_type, request.query_count,
                                                                       request.pipeline_statistics, pool);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = pool});
            }
            NTSTATUS handle_destroy_query_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_query_pool(request.device, request.object);
                return STATUS_SUCCESS;
            }
            NTSTATUS handle_reset_query_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::reset_query_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                const int32_t result =
                    this->vulkan_.reset_query_pool(request.device, request.query_pool, request.first_query, request.query_count);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }
            NTSTATUS handle_get_query_pool_results(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::get_query_pool_results_request;
                using response_t = gpu_bridge::get_query_pool_results_response;
                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const auto avail = static_cast<uint32_t>(context.output_buffer_length - sizeof(response_t));
                const auto data_size = std::min<uint32_t>(request.data_size, avail);
                std::vector<std::byte> data(data_size);
                size_t written = 0;
                const int32_t result =
                    this->vulkan_.get_query_pool_results(request.device, request.query_pool, request.first_query, request.query_count,
                                                         request.flags, data.data(), data.size(), request.stride, written);

                const response_t response{.vk_result = result, .data_size = static_cast<uint32_t>(written)};
                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response);
                if (written > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), data.data(), written);
                }
                set_information(context, static_cast<ULONG>(sizeof(response_t) + written));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_render_pass(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_render_pass_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t render_pass = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_render_pass(request.device, request.format, request.load_op, request.store_op,
                                                     request.initial_layout, request.final_layout, request.depth_format, render_pass);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = render_pass});
            }

            NTSTATUS handle_destroy_render_pass(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_render_pass(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_framebuffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_framebuffer_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t framebuffer = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_framebuffer(request.device, request.render_pass, request.image_view,
                                                                        request.depth_view, request.width, request.height, framebuffer);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = framebuffer});
            }

            NTSTATUS handle_destroy_framebuffer(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_framebuffer(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_pipeline_layout(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_pipeline_layout_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // The descriptor-set-layout ids trail the header.
                std::vector<uint64_t> set_layouts;
                if (!read_trailing_array(win_emu, context, sizeof(request_t), request.set_layout_count, set_layouts))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t layout = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_pipeline_layout(request.device, request.push_constant_stages,
                                                                            request.push_constant_size, set_layouts, layout);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = layout});
            }

            NTSTATUS handle_destroy_pipeline_layout(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_pipeline_layout(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_graphics_pipeline(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_graphics_pipeline_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // The vertex input state trails the header: binding_count vertex_input_binding entries
                // then attribute_count vertex_input_attribute entries. Bound the read by the buffer.
                const auto available = context.input_buffer_length - static_cast<uint32_t>(sizeof(request_t));
                std::vector<std::byte> trailer(available);
                if (available > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), trailer.data(), trailer.size());
                }

                const size_t bindings_bytes = static_cast<size_t>(request.binding_count) * sizeof(gpu_bridge::vertex_input_binding);
                const size_t attributes_bytes = static_cast<size_t>(request.attribute_count) * sizeof(gpu_bridge::vertex_input_attribute);
                if (bindings_bytes + attributes_bytes > trailer.size())
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<vulkan_host::vertex_binding> bindings(request.binding_count);
                for (uint32_t i = 0; i < request.binding_count; ++i)
                {
                    gpu_bridge::vertex_input_binding b{};
                    std::memcpy(&b, trailer.data() + i * sizeof(b), sizeof(b));
                    bindings[i] = {.binding = b.binding, .stride = b.stride, .input_rate = b.input_rate};
                }

                std::vector<vulkan_host::vertex_attribute> attributes(request.attribute_count);
                for (uint32_t i = 0; i < request.attribute_count; ++i)
                {
                    gpu_bridge::vertex_input_attribute a{};
                    std::memcpy(&a, trailer.data() + bindings_bytes + i * sizeof(a), sizeof(a));
                    attributes[i] = {.location = a.location, .binding = a.binding, .format = a.format, .offset = a.offset};
                }

                const vulkan_host::depth_state depth{.test_enable = request.depth_test_enable,
                                                     .write_enable = request.depth_write_enable,
                                                     .compare_op = request.depth_compare_op};

                uint64_t pipeline = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_graphics_pipeline(request.device, request.render_pass, request.pipeline_layout,
                                                                              request.vertex_shader, request.fragment_shader, request.width,
                                                                              request.height, bindings, attributes, depth, pipeline);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = pipeline});
            }

            NTSTATUS handle_destroy_pipeline(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_pipeline(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_descriptor_set_layout(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_descriptor_set_layout_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<gpu_bridge::descriptor_set_layout_binding> wire;
                if (!read_trailing_array(win_emu, context, sizeof(request_t), request.binding_count, wire))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<vulkan_host::descriptor_binding> bindings(wire.size());
                for (size_t i = 0; i < wire.size(); ++i)
                {
                    bindings[i] = {.binding = wire[i].binding,
                                   .descriptor_type = wire[i].descriptor_type,
                                   .descriptor_count = wire[i].descriptor_count,
                                   .stage_flags = wire[i].stage_flags};
                }

                uint64_t layout = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_descriptor_set_layout(request.device, bindings, layout);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = layout});
            }

            NTSTATUS handle_destroy_descriptor_set_layout(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_descriptor_set_layout(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_create_descriptor_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::create_descriptor_pool_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<gpu_bridge::descriptor_pool_size> wire;
                if (!read_trailing_array(win_emu, context, sizeof(request_t), request.pool_size_count, wire))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<vulkan_host::descriptor_pool_size> sizes(wire.size());
                for (size_t i = 0; i < wire.size(); ++i)
                {
                    sizes[i] = {.descriptor_type = wire[i].descriptor_type, .descriptor_count = wire[i].descriptor_count};
                }

                uint64_t pool = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_descriptor_pool(request.device, request.max_sets, sizes, pool);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = pool});
            }

            NTSTATUS handle_destroy_descriptor_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_descriptor_pool(request.device, request.object);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_allocate_descriptor_sets(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::allocate_descriptor_sets_request;
                using response_t = gpu_bridge::allocate_descriptor_sets_response;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                if (!context.output_buffer || context.output_buffer_length < sizeof(response_t))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::vector<uint64_t> set_layouts;
                if (!read_trailing_array(win_emu, context, sizeof(request_t), request.set_count, set_layouts))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto array_capacity =
                    static_cast<uint32_t>((context.output_buffer_length - sizeof(response_t)) / sizeof(gpu_bridge::object_id));
                std::vector<uint64_t> ids(std::min(request.set_count, array_capacity));
                uint32_t count = 0;
                const int32_t result =
                    this->vulkan_.allocate_descriptor_sets(request.device, request.descriptor_pool, set_layouts, std::span{ids}, count);

                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(response_t{.vk_result = result, .count = count});

                const auto written = std::min<uint32_t>(count, static_cast<uint32_t>(ids.size()));
                if (written > 0)
                {
                    win_emu.emu().write_memory(context.output_buffer + sizeof(response_t), ids.data(),
                                               written * sizeof(gpu_bridge::object_id));
                }
                set_information(context, static_cast<ULONG>(sizeof(response_t) + written * sizeof(gpu_bridge::object_id)));
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_update_descriptor_sets(windows_emulator& win_emu, const io_device_context& context)
            {
                using request_t = gpu_bridge::update_descriptor_sets_request;

                request_t request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<gpu_bridge::descriptor_write> wire;
                if (!read_trailing_array(win_emu, context, sizeof(request_t), request.write_count, wire))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<vulkan_host::descriptor_write> writes(wire.size());
                for (size_t i = 0; i < wire.size(); ++i)
                {
                    writes[i] = {.dst_set = wire[i].dst_set,
                                 .dst_binding = wire[i].dst_binding,
                                 .dst_array_element = wire[i].dst_array_element,
                                 .descriptor_type = wire[i].descriptor_type,
                                 .buffer = wire[i].buffer,
                                 .offset = wire[i].offset,
                                 .range = wire[i].range,
                                 .sampler = wire[i].sampler,
                                 .image_view = wire[i].image_view,
                                 .image_layout = wire[i].image_layout};
                }

                const int32_t result = this->vulkan_.update_descriptor_sets(request.device, writes);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_create_sampler(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_sampler_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t sampler = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_sampler(request.device, request.mag_filter, request.min_filter, request.address_mode_u,
                                                 request.address_mode_v, request.address_mode_w, sampler);
                return write_output(win_emu, context, gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = sampler});
            }

            NTSTATUS handle_destroy_sampler(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::device_child_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                this->vulkan_.destroy_sampler(request.device, request.object);
                return STATUS_SUCCESS;
            }

            // Executes one recorded command-buffer command from a batch (see ioctl_record_commands). The
            // payload is the command's normal request struct; this is the per-command core shared with the
            // (legacy) individual command IOCTL handlers. Returns the VkResult.
            int32_t execute_recorded_command(windows_emulator& win_emu, uint32_t command, const std::byte* payload, size_t size)
            {
                constexpr int32_t vk_error_initialization_failed = -3; // VK_ERROR_INITIALIZATION_FAILED (no vulkan.h here)
                const auto read = [&](auto& req) {
                    if (size < sizeof(req))
                    {
                        return false;
                    }
                    std::memcpy(&req, payload, sizeof(req));
                    return true;
                };

                switch (static_cast<gpu_bridge::command>(command))
                {
                case gpu_bridge::command::begin_command_buffer: {
                    gpu_bridge::begin_command_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.begin_command_buffer(req.command_buffer, req.flags);
                }
                case gpu_bridge::command::end_command_buffer: {
                    gpu_bridge::end_command_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.end_command_buffer(req.command_buffer);
                }
                case gpu_bridge::command::cmd_begin_render_pass: {
                    gpu_bridge::cmd_begin_render_pass_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_begin_render_pass(req.command_buffer, req.render_pass, req.framebuffer, req.width, req.height,
                                                               req.clear_r, req.clear_g, req.clear_b, req.clear_a, req.clear_depth);
                }
                case gpu_bridge::command::cmd_bind_pipeline: {
                    gpu_bridge::cmd_bind_pipeline_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_bind_pipeline(req.command_buffer, req.pipeline, req.bind_point);
                }
                case gpu_bridge::command::cmd_copy_buffer: {
                    gpu_bridge::cmd_copy_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const size_t regions_bytes = static_cast<size_t>(req.region_count) * sizeof(gpu_bridge::buffer_copy_region);
                    if (regions_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<vulkan_host::buffer_copy> regions(req.region_count);
                    for (uint32_t i = 0; i < req.region_count; ++i)
                    {
                        gpu_bridge::buffer_copy_region r{};
                        std::memcpy(&r, payload + sizeof(req) + i * sizeof(r), sizeof(r));
                        regions[i] = vulkan_host::buffer_copy{.src_offset = r.src_offset, .dst_offset = r.dst_offset, .size = r.size};
                    }
                    return this->vulkan_.cmd_copy_buffer(req.command_buffer, req.src_buffer, req.dst_buffer, regions);
                }
                case gpu_bridge::command::cmd_reset_query_pool: {
                    gpu_bridge::cmd_reset_query_pool_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_reset_query_pool(req.command_buffer, req.query_pool, req.first_query, req.query_count);
                }
                case gpu_bridge::command::cmd_begin_query: {
                    gpu_bridge::cmd_begin_query_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_begin_query(req.command_buffer, req.query_pool, req.query, req.flags);
                }
                case gpu_bridge::command::cmd_end_query: {
                    gpu_bridge::cmd_end_query_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_end_query(req.command_buffer, req.query_pool, req.query);
                }
                case gpu_bridge::command::cmd_write_timestamp: {
                    gpu_bridge::cmd_write_timestamp_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_write_timestamp(req.command_buffer, req.query_pool, req.query, req.pipeline_stage);
                }
                case gpu_bridge::command::cmd_dispatch: {
                    gpu_bridge::cmd_dispatch_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_dispatch(req.command_buffer, req.group_count_x, req.group_count_y, req.group_count_z);
                }
                case gpu_bridge::command::cmd_dispatch_indirect: {
                    gpu_bridge::cmd_dispatch_indirect_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_dispatch_indirect(req.command_buffer, req.buffer, req.offset);
                }
                case gpu_bridge::command::cmd_push_constants: {
                    gpu_bridge::cmd_push_constants_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const auto data_bytes = std::min<uint64_t>(req.size, size - sizeof(req));
                    return this->vulkan_.cmd_push_constants(req.command_buffer, req.pipeline_layout, req.stage_flags, req.offset,
                                                            static_cast<uint32_t>(data_bytes), payload + sizeof(req));
                }
                case gpu_bridge::command::cmd_draw: {
                    gpu_bridge::cmd_draw_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_draw(req.command_buffer, req.vertex_count, req.instance_count, req.first_vertex,
                                                  req.first_instance);
                }
                case gpu_bridge::command::cmd_bind_vertex_buffers: {
                    gpu_bridge::cmd_bind_vertex_buffers_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const size_t bindings_bytes = static_cast<size_t>(req.binding_count) * sizeof(gpu_bridge::vertex_buffer_binding);
                    if (bindings_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<uint64_t> buffer_ids(req.binding_count);
                    std::vector<uint64_t> offsets(req.binding_count);
                    for (uint32_t i = 0; i < req.binding_count; ++i)
                    {
                        gpu_bridge::vertex_buffer_binding vb{};
                        std::memcpy(&vb, payload + sizeof(req) + i * sizeof(vb), sizeof(vb));
                        buffer_ids[i] = vb.buffer;
                        offsets[i] = vb.offset;
                    }
                    return this->vulkan_.cmd_bind_vertex_buffers(req.command_buffer, req.first_binding, req.binding_count,
                                                                 buffer_ids.data(), offsets.data());
                }
                case gpu_bridge::command::cmd_bind_index_buffer: {
                    gpu_bridge::cmd_bind_index_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_bind_index_buffer(req.command_buffer, req.buffer, req.offset, req.index_type);
                }
                case gpu_bridge::command::cmd_draw_indexed: {
                    gpu_bridge::cmd_draw_indexed_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_draw_indexed(req.command_buffer, req.index_count, req.instance_count, req.first_index,
                                                          req.vertex_offset, req.first_instance);
                }
                case gpu_bridge::command::cmd_bind_descriptor_sets: {
                    gpu_bridge::cmd_bind_descriptor_sets_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const size_t ids_bytes = static_cast<size_t>(req.set_count) * sizeof(gpu_bridge::object_id);
                    if (ids_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<uint64_t> sets(req.set_count);
                    if (req.set_count > 0)
                    {
                        std::memcpy(sets.data(), payload + sizeof(req), ids_bytes);
                    }
                    return this->vulkan_.cmd_bind_descriptor_sets(req.command_buffer, req.pipeline_layout, req.first_set, sets,
                                                                  req.bind_point);
                }
                case gpu_bridge::command::cmd_end_render_pass: {
                    gpu_bridge::cmd_end_render_pass_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_end_render_pass(req.command_buffer);
                }
                case gpu_bridge::command::cmd_fill_buffer: {
                    gpu_bridge::cmd_fill_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_fill_buffer(req.command_buffer, req.buffer, req.offset, req.size, req.data);
                }
                case gpu_bridge::command::cmd_pipeline_barrier: {
                    gpu_bridge::cmd_pipeline_barrier_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_pipeline_barrier(req.command_buffer, req.image, req.src_stage_mask, req.dst_stage_mask,
                                                              req.src_access_mask, req.dst_access_mask, req.old_layout, req.new_layout,
                                                              to_host_range(req.subresource));
                }
                case gpu_bridge::command::cmd_clear_color_image: {
                    gpu_bridge::cmd_clear_color_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_clear_color_image(req.command_buffer, req.image, req.image_layout, req.color_r, req.color_g,
                                                               req.color_b, req.color_a, to_host_range(req.subresource));
                }
                case gpu_bridge::command::cmd_clear_depth_stencil_image: {
                    gpu_bridge::cmd_clear_depth_stencil_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_clear_depth_stencil_image(req.command_buffer, req.image, req.image_layout, req.depth,
                                                                       req.stencil, to_host_range(req.subresource));
                }
                case gpu_bridge::command::cmd_copy_image_to_buffer: {
                    gpu_bridge::cmd_copy_image_to_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_copy_image_to_buffer(req.command_buffer, req.image, req.image_layout, req.buffer, req.width,
                                                                  req.height, req.aspect_mask);
                }
                case gpu_bridge::command::cmd_copy_buffer_to_image: {
                    gpu_bridge::cmd_copy_buffer_to_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_copy_buffer_to_image(req.command_buffer, req.buffer, req.image, req.image_layout, req.width,
                                                                  req.height, req.aspect_mask);
                }
                default:
                    win_emu.log.warn("[gpu-bridge] record_commands: unsupported command 0x%X\n", command);
                    return vk_error_initialization_failed;
                }
            }

            // Replays a batched command-buffer recording: one IOCTL carries the whole begin->cmds->end
            // stream, amortising the per-command boundary crossing. See ioctl_record_commands.
            NTSTATUS handle_record_commands(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length == 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> stream(context.input_buffer_length);
                win_emu.emu().read_memory(context.input_buffer, stream.data(), stream.size());

                int32_t result = 0; // VK_SUCCESS
                size_t offset = 0;
                while (offset + sizeof(gpu_bridge::command_record_header) <= stream.size())
                {
                    gpu_bridge::command_record_header header{};
                    std::memcpy(&header, stream.data() + offset, sizeof(header));
                    offset += sizeof(header);
                    if (header.size > stream.size() - offset)
                    {
                        break; // truncated / malformed record
                    }

                    const int32_t r = this->execute_recorded_command(win_emu, header.command, stream.data() + offset, header.size);
                    if (r != 0 && result == 0)
                    {
                        result = r; // report the first failure
                    }
                    offset += header.size;
                }

                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_get_surface_capabilities(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_surface_capabilities_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (!context.output_buffer || context.output_buffer_length == 0)
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                std::vector<std::byte> caps(context.output_buffer_length);
                const int32_t result =
                    this->vulkan_.get_surface_capabilities(request.physical_device, request.surface, caps.data(), caps.size());
                if (result != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                win_emu.emu().write_memory(context.output_buffer, caps.data(), caps.size());
                set_information(context, context.output_buffer_length);
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_gpu_bridge()
    {
        return std::make_unique<gpu_bridge_device>();
    }
}
