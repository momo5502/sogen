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
                case gpu_bridge::ioctl_destroy_image:
                    return handle_destroy_image(win_emu, context);
                case gpu_bridge::ioctl_get_image_memory_requirements:
                    return handle_get_image_memory_requirements(win_emu, context);
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

                default:
                    win_emu.log.warn("[gpu-bridge] Unsupported IOCTL: 0x%X\n", context.io_control_code);
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
                    context.io_status_block.access(
                        [&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = bytes; });
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

                const auto request =
                    emulator_object<gpu_bridge::destroy_instance_request>{win_emu.emu(), context.input_buffer}.read();

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
                const int32_t result =
                    this->vulkan_.get_physical_device_properties(request.physical_device, properties.data(), properties.size());
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

                uint64_t device = gpu_bridge::null_object;
                const int32_t result =
                    this->vulkan_.create_device(request.physical_device, request.queue_family_index, request.queue_count, device);

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

                const auto request =
                    emulator_object<gpu_bridge::destroy_device_request>{win_emu.emu(), context.input_buffer}.read();

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
                const int32_t result =
                    this->vulkan_.create_command_pool(request.device, request.queue_family_index, request.flags, pool);
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
                const int32_t result = this->vulkan_.get_physical_device_memory_properties(request.physical_device,
                                                                                           properties.data(), properties.size());
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
                const int32_t result =
                    this->vulkan_.allocate_memory(request.device, request.size, request.memory_type_index, memory);
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
                const int32_t result = this->vulkan_.get_buffer_memory_requirements(request.device, request.buffer, size,
                                                                                    alignment, memory_type_bits);
                return write_output(win_emu, context,
                                    gpu_bridge::memory_requirements_response{.vk_result = result,
                                                                             .memory_type_bits = memory_type_bits,
                                                                             .size = size,
                                                                             .alignment = alignment});
            }

            NTSTATUS handle_bind_buffer_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::bind_buffer_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.bind_buffer_memory(request.device, request.buffer, request.memory, request.offset);
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

                std::vector<std::byte> bytes(copy_bytes);
                const int32_t result = this->vulkan_.download_memory(request.device, request.memory, request.offset,
                                                                     copy_bytes, bytes.data(), bytes.size());
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

                std::vector<std::byte> bytes(payload);
                if (payload > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), bytes.data(), bytes.size());
                }

                const int32_t result = this->vulkan_.upload_memory(request.device, request.memory, request.offset, payload,
                                                                    bytes.data(), bytes.size());
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
                const int32_t result = this->vulkan_.create_image(request.device, request.format, request.width,
                                                                  request.height, request.usage, request.tiling, image);
                return write_output(win_emu, context,
                                    gpu_bridge::create_image_response{.vk_result = result, .reserved = 0, .image = image});
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
                const int32_t result = this->vulkan_.get_image_memory_requirements(request.device, request.image, size,
                                                                                   alignment, memory_type_bits);
                return write_output(win_emu, context,
                                    gpu_bridge::memory_requirements_response{.vk_result = result,
                                                                             .memory_type_bits = memory_type_bits,
                                                                             .size = size,
                                                                             .alignment = alignment});
            }

            NTSTATUS handle_bind_image_memory(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::bind_image_memory_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.bind_image_memory(request.device, request.image, request.memory, request.offset);
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
                    this->vulkan_.create_swapchain(request.device, request.surface, request.format, request.width,
                                                   request.height, request.min_image_count, request.image_usage, swapchain,
                                                   image_count);
                return write_output(win_emu, context,
                                    gpu_bridge::create_swapchain_response{
                                        .vk_result = result, .image_count = image_count, .swapchain = swapchain});
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

                emulator_object<response_t>{win_emu.emu(), context.output_buffer}.write(
                    response_t{.vk_result = result, .count = count});

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
                return write_output(win_emu, context,
                                    gpu_bridge::acquire_next_image_response{.vk_result = result, .image_index = index});
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
                    this->vulkan_.queue_present(request.queue, request.swapchain, request.image_index, pixels, width, height,
                                                hwnd_value);

                // Hand the freshly read-back pixels to the guest window through the UI backend (the same
                // seam GDI EndPaint uses). The swapchain is B8G8R8A8, matching bgra8, so no swizzle.
                if (result == 0 /* VK_SUCCESS */ && hwnd_value != 0 && !pixels.empty())
                {
                    win_emu.ui().present_surface(static_cast<hwnd>(hwnd_value),
                                                 ui_surface_desc{
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
                std::vector<std::byte> code(code_bytes);
                if (code_bytes > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), code.data(), code.size());
                }

                uint64_t module = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_shader_module(request.device, code.data(), code.size(), module);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = module});
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
                const int32_t result = this->vulkan_.create_image_view(request.device, request.image, request.format, view);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = view});
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

            NTSTATUS handle_create_render_pass(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_render_pass_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t render_pass = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_render_pass(request.device, request.format, request.load_op,
                                                                        request.store_op, request.initial_layout,
                                                                        request.final_layout, render_pass);
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
                const int32_t result = this->vulkan_.create_framebuffer(request.device, request.render_pass,
                                                                        request.image_view, request.width, request.height,
                                                                        framebuffer);
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
                gpu_bridge::create_pipeline_layout_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t layout = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_pipeline_layout(
                    request.device, request.push_constant_stages, request.push_constant_size, layout);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = layout});
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
                gpu_bridge::create_graphics_pipeline_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }
                uint64_t pipeline = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_graphics_pipeline(
                    request.device, request.render_pass, request.pipeline_layout, request.vertex_shader,
                    request.fragment_shader, request.width, request.height, pipeline);
                return write_output(win_emu, context,
                                    gpu_bridge::object_response{.vk_result = result, .reserved = 0, .object = pipeline});
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
                case gpu_bridge::command::begin_command_buffer:
                {
                    gpu_bridge::begin_command_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.begin_command_buffer(req.command_buffer, req.flags);
                }
                case gpu_bridge::command::end_command_buffer:
                {
                    gpu_bridge::end_command_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.end_command_buffer(req.command_buffer);
                }
                case gpu_bridge::command::cmd_begin_render_pass:
                {
                    gpu_bridge::cmd_begin_render_pass_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_begin_render_pass(req.command_buffer, req.render_pass, req.framebuffer, req.width,
                                                               req.height, req.clear_r, req.clear_g, req.clear_b, req.clear_a);
                }
                case gpu_bridge::command::cmd_bind_pipeline:
                {
                    gpu_bridge::cmd_bind_pipeline_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_bind_pipeline(req.command_buffer, req.pipeline);
                }
                case gpu_bridge::command::cmd_push_constants:
                {
                    gpu_bridge::cmd_push_constants_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const auto data_bytes = std::min<uint64_t>(req.size, size - sizeof(req));
                    return this->vulkan_.cmd_push_constants(req.command_buffer, req.pipeline_layout, req.stage_flags, req.offset,
                                                            static_cast<uint32_t>(data_bytes), payload + sizeof(req));
                }
                case gpu_bridge::command::cmd_draw:
                {
                    gpu_bridge::cmd_draw_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_draw(req.command_buffer, req.vertex_count, req.instance_count, req.first_vertex,
                                                  req.first_instance);
                }
                case gpu_bridge::command::cmd_end_render_pass:
                {
                    gpu_bridge::cmd_end_render_pass_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_end_render_pass(req.command_buffer);
                }
                case gpu_bridge::command::cmd_fill_buffer:
                {
                    gpu_bridge::cmd_fill_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_fill_buffer(req.command_buffer, req.buffer, req.offset, req.size, req.data);
                }
                case gpu_bridge::command::cmd_pipeline_barrier:
                {
                    gpu_bridge::cmd_pipeline_barrier_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_pipeline_barrier(req.command_buffer, req.image, req.src_stage_mask,
                                                              req.dst_stage_mask, req.src_access_mask, req.dst_access_mask,
                                                              req.old_layout, req.new_layout, to_host_range(req.subresource));
                }
                case gpu_bridge::command::cmd_clear_color_image:
                {
                    gpu_bridge::cmd_clear_color_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_clear_color_image(req.command_buffer, req.image, req.image_layout, req.color_r,
                                                               req.color_g, req.color_b, req.color_a,
                                                               to_host_range(req.subresource));
                }
                case gpu_bridge::command::cmd_copy_image_to_buffer:
                {
                    gpu_bridge::cmd_copy_image_to_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_copy_image_to_buffer(req.command_buffer, req.image, req.image_layout, req.buffer,
                                                                  req.width, req.height, req.aspect_mask);
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
                const int32_t result = this->vulkan_.get_surface_capabilities(request.physical_device, request.surface,
                                                                              caps.data(), caps.size());
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
