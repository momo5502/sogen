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
            void work(windows_emulator& win_emu) override
            {
                for (auto& frame : this->vulkan_.poll_presented_frames())
                {
                    present_surface_if_ready(win_emu, frame.hwnd, frame.width, frame.height, frame.pixels);
                }
            }

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
                case gpu_bridge::ioctl_create_event:
                    return handle_create_event(win_emu, context);
                case gpu_bridge::ioctl_destroy_event:
                    return handle_destroy_event(win_emu, context);
                case gpu_bridge::ioctl_get_event_status:
                    return handle_get_event_status(win_emu, context);
                case gpu_bridge::ioctl_set_event:
                    return handle_set_event(win_emu, context);
                case gpu_bridge::ioctl_reset_event:
                    return handle_reset_event(win_emu, context);
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
                case gpu_bridge::ioctl_get_physical_device_memory_budget:
                    return handle_get_physical_device_memory_budget(win_emu, context);
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
                case gpu_bridge::ioctl_map_memory_direct:
                    return handle_map_memory_direct(win_emu, context);
                case gpu_bridge::ioctl_unmap_memory_direct:
                    return handle_unmap_memory_direct(win_emu, context);
                case gpu_bridge::ioctl_flush_mapped_memory_direct:
                    return handle_flush_mapped_memory_direct(win_emu, context);
                case gpu_bridge::ioctl_invalidate_mapped_memory_direct:
                    return handle_invalidate_mapped_memory_direct(win_emu, context);
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
                case gpu_bridge::ioctl_reset_descriptor_pool:
                    return handle_reset_descriptor_pool(win_emu, context);
                case gpu_bridge::ioctl_allocate_descriptor_sets:
                    return handle_allocate_descriptor_sets(win_emu, context);
                case gpu_bridge::ioctl_update_descriptor_sets:
                    return handle_update_descriptor_sets(win_emu, context);
                case gpu_bridge::ioctl_update_descriptor_sets_batch:
                    return handle_update_descriptor_sets_batch(win_emu, context);
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

            // VkDeviceMemory aliased directly into the guest address space (see handle_map_memory_direct),
            // keyed by memory object id, so unmap can release the guest range and the host mapping.
            struct direct_mapping
            {
                uint64_t guest_address{};
                uint64_t size{};
                uint64_t device{};
                void* host_ptr{};
            };
            std::unordered_map<uint64_t, direct_mapping> direct_mappings_{};

            // Before the host GPU reads guest-produced data, make the guest's writes to every directly-aliased
            // buffer visible. On backends that alias host memory non-coherently (KVM: guest writes are
            // write-back cached while the GPU may read write-combined memory) this evicts the CPU cache for the
            // aliased ranges; on coherent backends it is a no-op skipped by the capability check.
            void flush_aliased_memory_for_device(windows_emulator& win_emu) const
            {
                if (win_emu.memory.host_memory_aliasing_is_coherent())
                {
                    return;
                }

                for (const auto& [id, mapping] : this->direct_mappings_)
                {
                    if (mapping.host_ptr != nullptr && mapping.size != 0)
                    {
                        win_emu.memory.flush_host_memory_cache(mapping.host_ptr, static_cast<size_t>(mapping.size));
                    }
                }
            }

            static void set_information(const io_device_context& context, const ULONG bytes)
            {
                if (context.io_status_block)
                {
                    context.io_status_block.access([&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = bytes; });
                }
            }

            static void present_surface_if_ready(windows_emulator& win_emu, const uint64_t hwnd_value, const uint32_t width,
                                                 const uint32_t height, const std::vector<std::byte>& pixels)
            {
                if (hwnd_value == 0 || pixels.empty())
                {
                    return;
                }

                win_emu.ui().present_surface(static_cast<hwnd>(hwnd_value), ui_surface_desc{
                                                                                .width = static_cast<int>(width),
                                                                                .height = static_cast<int>(height),
                                                                                .stride = static_cast<int>(width * 4),
                                                                                .format = ui_surface_format::bgra8,
                                                                                .pixels = pixels.data(),
                                                                            });
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

                // destroy_instance internally tears down all devices for this instance via erase_device,
                // which frees Vulkan memory without consulting direct_mappings_. Release all guest VA
                // aliases first to prevent stale mappings to freed host pages.
                for (auto it = this->direct_mappings_.begin(); it != this->direct_mappings_.end();)
                {
                    win_emu.memory.release_memory(it->second.guest_address, static_cast<size_t>(it->second.size));
                    this->vulkan_.unmap_memory(it->second.device, it->first);
                    it = this->direct_mappings_.erase(it);
                }

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
                const int32_t result = this->vulkan_.get_queue_family_properties(
                    request.physical_device, request.query_ownership_transfer != 0, properties.data(), properties.size(), count);
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

                // Read the trailing [queue entries][extension names][feature records] blobs.
                const auto trailing = context.input_buffer_length - static_cast<uint32_t>(sizeof(request_t));
                std::vector<std::byte> blob(trailing);
                if (trailing > 0)
                {
                    win_emu.emu().read_memory(context.input_buffer + sizeof(request_t), blob.data(), trailing);
                }

                const size_t queue_bytes = static_cast<size_t>(request.queue_create_count) * sizeof(gpu_bridge::device_queue_create_entry);

                std::vector<gpu_bridge::device_queue_create_entry> queue_entries;
                const std::byte* extension_blob = nullptr;
                const std::byte* feature_blob = nullptr;
                uint32_t extension_count = 0;
                uint32_t feature_struct_count = 0;
                size_t extension_blob_size = 0;
                size_t feature_blob_size = 0;
                if (queue_bytes + static_cast<size_t>(request.extension_blob_size) + request.feature_blob_size <= blob.size())
                {
                    queue_entries.resize(request.queue_create_count);
                    if (queue_bytes > 0)
                    {
                        std::memcpy(queue_entries.data(), blob.data(), queue_bytes);
                    }
                    extension_blob = blob.data() + queue_bytes;
                    extension_blob_size = request.extension_blob_size;
                    extension_count = request.extension_count;
                    feature_blob = blob.data() + queue_bytes + request.extension_blob_size;
                    feature_blob_size = request.feature_blob_size;
                    feature_struct_count = request.feature_struct_count;
                }

                uint64_t device = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_device(request.physical_device, queue_entries.data(), queue_entries.size(),
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

                for (auto it = this->direct_mappings_.begin(); it != this->direct_mappings_.end();)
                {
                    if (it->second.device != request.device)
                    {
                        ++it;
                        continue;
                    }

                    win_emu.memory.release_memory(it->second.guest_address, static_cast<size_t>(it->second.size));
                    this->vulkan_.unmap_memory(it->second.device, it->first);
                    it = this->direct_mappings_.erase(it);
                }

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
                const int32_t result =
                    this->vulkan_.allocate_command_buffer(request.device, request.command_pool, request.level, command_buffer);
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

            NTSTATUS handle_create_event(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::create_event_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                uint64_t event = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_event(request.device, request.flags, event);
                return write_output(win_emu, context,
                                    gpu_bridge::create_event_response{.vk_result = result, .reserved = 0, .event = event});
            }

            NTSTATUS handle_destroy_event(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::destroy_event_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                this->vulkan_.destroy_event(request.device, request.event);
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_get_event_status(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_event_status_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.get_event_status(request.event);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_set_event(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::event_op_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.set_event(request.device, request.event);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_reset_event(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::event_op_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.reset_event(request.device, request.event);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
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

                constexpr int32_t vk_success = 0;
                constexpr int32_t vk_timeout = 2;

                const auto device = request.device;
                const auto flags = request.flags;
                const auto count = request.semaphore_count;

                const int32_t poll = this->vulkan_.wait_semaphores(device, flags, entries.data(), count, 0);

                // Signaled, errored, or a finite-timeout wait: handle inline. The hot path is DXVK's infinite
                // frame waits, parked cooperatively below.
                if (poll != vk_timeout || request.timeout != UINT64_MAX)
                {
                    const int32_t result =
                        (poll == vk_timeout) ? this->vulkan_.wait_semaphores(device, flags, entries.data(), count, request.timeout) : poll;
                    return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
                }

                // Infinite wait, not yet signaled: blocking would freeze the single VP and every guest thread.
                // Park on a semaphore-polling predicate instead, so the scheduler runs other threads and wakes
                // this one when the GPU signals.
                const auto out_status = write_output(win_emu, context, gpu_bridge::result_response{.vk_result = vk_success, .reserved = 0});
                if (out_status != STATUS_SUCCESS)
                {
                    return out_status;
                }

                auto* vulkan = &this->vulkan_;
                context.thread().await_host_condition = [vulkan, &win_emu, context, device, flags, entries = std::move(entries), count]() {
                    const int32_t result = vulkan->wait_semaphores(device, flags, entries.data(), count, 0);
                    if (result == vk_timeout)
                    {
                        return false;
                    }
                    // Propagate the real result so an error (e.g. device loss) reaches the guest instead of hanging.
                    write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
                    return true;
                };
                win_emu.yield_thread(*context.vcpu, false);
                return STATUS_SUCCESS;
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

                this->flush_aliased_memory_for_device(win_emu);

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

                this->flush_aliased_memory_for_device(win_emu);

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

            NTSTATUS handle_get_physical_device_memory_budget(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::get_physical_device_memory_budget_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                gpu_bridge::get_physical_device_memory_budget_response response{};
                uint32_t heap_count = 0;
                response.vk_result =
                    this->vulkan_.get_physical_device_memory_budget(request.physical_device, response.heap_budget.data(),
                                                                    response.heap_usage.data(), gpu_bridge::max_memory_heaps, heap_count);
                response.heap_count = heap_count;
                return write_output(win_emu, context, response);
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

                // If this allocation was aliased into the guest, tear the alias down and forget the host
                // pointer before vkFreeMemory invalidates it. Otherwise the guest keeps a stale alias of freed
                // memory and direct_mappings_ retains a dangling pointer that the pre-submit cache flush would
                // dereference (e.g. while DXVK frees buffers during shutdown).
                if (const auto it = this->direct_mappings_.find(request.memory); it != this->direct_mappings_.end())
                {
                    win_emu.memory.release_memory(it->second.guest_address, static_cast<size_t>(it->second.size));
                    this->vulkan_.unmap_memory(it->second.device, request.memory);
                    this->direct_mappings_.erase(it);
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
                const auto direct = this->direct_mappings_.find(request.memory);
                // The alias only covers whole-page allocations, so direct->second.size is the real allocation
                // size; bound the copy against it without overflowing on a hostile offset.
                if (direct != this->direct_mappings_.end() && direct->second.host_ptr && request.offset <= direct->second.size &&
                    copy_bytes <= direct->second.size - request.offset)
                {
                    std::memcpy(bytes.data(), static_cast<const std::byte*>(direct->second.host_ptr) + request.offset,
                                static_cast<size_t>(copy_bytes));
                }
                else
                {
                    const int32_t result = this->vulkan_.download_memory(request.device, request.memory, request.offset, copy_bytes,
                                                                         bytes.data(), bytes.size());
                    if (result != 0)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
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

            // Maps the host VkDeviceMemory and aliases it straight into the guest address space, so the guest
            // accesses it coherently with no staging copy. Allocations are page-rounded at allocation time, so
            // the page-granular alias never covers memory beyond the allocation. Returns guest_address = 0 if it
            // still can't be aliased (e.g. an unaligned host pointer, or -- as a safety net -- a non-page-sized
            // allocation), in which case the shim falls back to the (bounds-checked) staging path.
            NTSTATUS handle_map_memory_direct(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::map_memory_direct_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                gpu_bridge::map_memory_direct_response response{};

                // The whole VkDeviceMemory object is aliased once into the guest; any sub-range map is just
                // an offset into that single coherent alias. If it's already aliased, return base + offset.
                if (const auto existing = this->direct_mappings_.find(request.memory); existing != this->direct_mappings_.end())
                {
                    response.vk_result = 0; // VK_SUCCESS
                    response.guest_address = existing->second.guest_address + request.offset;
                    return write_output(win_emu, context, response);
                }

                void* host_ptr = nullptr;
                uint64_t host_size = 0;
                response.vk_result = this->vulkan_.map_memory(request.device, request.memory, host_ptr, host_size);

                constexpr uint64_t page = 0x1000;
                if (response.vk_result != 0 || !host_ptr || host_size == 0 || (reinterpret_cast<uintptr_t>(host_ptr) % page) != 0 ||
                    (host_size % page) != 0)
                {
                    if (host_ptr)
                    {
                        this->vulkan_.unmap_memory(request.device, request.memory);
                    }
                    return write_output(win_emu, context, response);
                }

                const uint64_t mapped_size = (host_size + page - 1) & ~(page - 1);
                const uint64_t va = win_emu.memory.find_free_allocation_base(static_cast<size_t>(mapped_size));
                if (va == 0 ||
                    !win_emu.memory.allocate_host_memory(va, static_cast<size_t>(mapped_size), host_ptr, memory_permission::read_write))
                {
                    this->vulkan_.unmap_memory(request.device, request.memory);
                    return write_output(win_emu, context, response);
                }

                this->direct_mappings_[request.memory] =
                    direct_mapping{.guest_address = va, .size = mapped_size, .device = request.device, .host_ptr = host_ptr};
                response.guest_address = va + request.offset;
                return write_output(win_emu, context, response);
            }

            NTSTATUS handle_unmap_memory_direct(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::unmap_memory_direct_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const auto it = this->direct_mappings_.find(request.memory);
                if (it != this->direct_mappings_.end())
                {
                    win_emu.memory.release_memory(it->second.guest_address, static_cast<size_t>(it->second.size));
                    this->vulkan_.unmap_memory(it->second.device, request.memory);
                    this->direct_mappings_.erase(it);
                }
                return STATUS_SUCCESS;
            }

            NTSTATUS handle_flush_mapped_memory_direct(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::mapped_memory_range_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.flush_mapped_memory_range(request.device, request.memory, request.offset, request.size);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            NTSTATUS handle_invalidate_mapped_memory_direct(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::mapped_memory_range_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result =
                    this->vulkan_.invalidate_mapped_memory_range(request.device, request.memory, request.offset, request.size);
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
                const int32_t result = this->vulkan_.create_image(
                    request.device, request.format, request.width, request.height, request.usage, request.tiling, request.samples,
                    request.image_type, request.depth, request.mip_levels, request.array_layers, request.flags, image);
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
                const int32_t result = this->vulkan_.acquire_next_image(request.swapchain, request.semaphore, request.fence, index);
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
                if (result == 0 /* VK_SUCCESS */)
                {
                    present_surface_if_ready(win_emu, hwnd_value, width, height, pixels);
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
                const int32_t result = this->vulkan_.create_image_view(request.device, request.image, request.format, request.aspect_mask,
                                                                       request.view_type, request.base_mip_level, request.level_count,
                                                                       request.base_array_layer, request.layer_count, request.swizzle_r,
                                                                       request.swizzle_g, request.swizzle_b, request.swizzle_a, view);
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
                size_t binding_offset = 0;
                for (auto& binding : bindings)
                {
                    gpu_bridge::vertex_input_binding b{};
                    std::memcpy(&b, trailer.data() + binding_offset, sizeof(b));
                    binding_offset += sizeof(b);
                    binding = {.binding = b.binding, .stride = b.stride, .input_rate = b.input_rate};
                }

                std::vector<vulkan_host::vertex_attribute> attributes(request.attribute_count);
                size_t attribute_offset = bindings_bytes;
                for (auto& attribute : attributes)
                {
                    gpu_bridge::vertex_input_attribute a{};
                    std::memcpy(&a, trailer.data() + attribute_offset, sizeof(a));
                    attribute_offset += sizeof(a);
                    attribute = {.location = a.location, .binding = a.binding, .format = a.format, .offset = a.offset};
                }

                std::vector<uint32_t> dynamic_states(request.dynamic_state_count);
                const size_t dynamic_bytes = static_cast<size_t>(request.dynamic_state_count) * sizeof(uint32_t);
                if (bindings_bytes + attributes_bytes + dynamic_bytes <= trailer.size() && request.dynamic_state_count > 0)
                {
                    std::memcpy(dynamic_states.data(), trailer.data() + bindings_bytes + attributes_bytes, dynamic_bytes);
                }

                // The two per-stage specialization-constant blocks (vertex then fragment) trail the dynamic
                // states: each is `entry_count` specialization_map_entry records followed by `data_size` bytes.
                size_t spec_cursor = bindings_bytes + attributes_bytes + dynamic_bytes;
                std::vector<vulkan_host::spec_entry> vs_entries;
                std::vector<vulkan_host::spec_entry> fs_entries;
                std::vector<uint8_t> vs_data;
                std::vector<uint8_t> fs_data;
                const auto parse_spec = [&](uint32_t entry_count, uint32_t data_size, std::vector<vulkan_host::spec_entry>& entries,
                                            std::vector<uint8_t>& data) {
                    const size_t entries_bytes = static_cast<size_t>(entry_count) * sizeof(gpu_bridge::specialization_map_entry);
                    if (spec_cursor + entries_bytes + data_size > trailer.size())
                    {
                        return;
                    }
                    entries.resize(entry_count);
                    for (auto& entry : entries)
                    {
                        gpu_bridge::specialization_map_entry e{};
                        std::memcpy(&e, trailer.data() + spec_cursor, sizeof(e));
                        spec_cursor += sizeof(e);
                        entry = {.constant_id = e.constant_id, .offset = e.offset, .size = e.size};
                    }
                    data.resize(data_size);
                    if (data_size > 0)
                    {
                        std::memcpy(data.data(), trailer.data() + spec_cursor, data_size);
                        spec_cursor += data_size;
                    }
                };
                parse_spec(request.vs_spec_entry_count, request.vs_spec_data_size, vs_entries, vs_data);
                parse_spec(request.fs_spec_entry_count, request.fs_spec_data_size, fs_entries, fs_data);

                const vulkan_host::depth_state depth{.test_enable = request.depth_test_enable,
                                                     .write_enable = request.depth_write_enable,
                                                     .compare_op = request.depth_compare_op};

                const uint32_t color_count = std::min<uint32_t>(request.color_attachment_count, gpu_bridge::max_color_attachments);
                const std::span<const uint32_t> color_formats{request.color_formats.data(), color_count};

                const vulkan_host::specialization vs_spec{.entries = vs_entries, .data = vs_data};
                const vulkan_host::specialization fs_spec{.entries = fs_entries, .data = fs_data};

                const uint32_t blend_count = std::min<uint32_t>(request.blend_attachment_count, gpu_bridge::max_color_attachments);
                std::vector<vulkan_host::color_blend_attachment> blend_attachments(blend_count);
                for (uint32_t i = 0; i < blend_count; ++i)
                {
                    const gpu_bridge::pipeline_blend_attachment& b = request.blend_attachments.at(i);
                    blend_attachments.at(i) = {.blend_enable = b.blend_enable,
                                               .src_color_blend_factor = b.src_color_blend_factor,
                                               .dst_color_blend_factor = b.dst_color_blend_factor,
                                               .color_blend_op = b.color_blend_op,
                                               .src_alpha_blend_factor = b.src_alpha_blend_factor,
                                               .dst_alpha_blend_factor = b.dst_alpha_blend_factor,
                                               .alpha_blend_op = b.alpha_blend_op,
                                               .color_write_mask = b.color_write_mask};
                }

                uint64_t pipeline = gpu_bridge::null_object;
                const int32_t result = this->vulkan_.create_graphics_pipeline(
                    request.device, request.render_pass, request.pipeline_layout, request.vertex_shader, request.fragment_shader,
                    request.width, request.height, bindings, attributes, depth, color_formats, request.depth_format, request.stencil_format,
                    request.rasterization_samples, request.primitive_topology, request.primitive_restart_enable, dynamic_states, vs_spec,
                    fs_spec, blend_attachments, pipeline);
                if (result != 0)
                {
                    win_emu.log.error(
                        "GPU bridge: create_graphics_pipeline FAILED vk=%d (rp=0x%llx layout=0x%llx vs=0x%llx fs=0x%llx %ux%u)\n", result,
                        static_cast<unsigned long long>(request.render_pass), static_cast<unsigned long long>(request.pipeline_layout),
                        static_cast<unsigned long long>(request.vertex_shader), static_cast<unsigned long long>(request.fragment_shader),
                        request.width, request.height);
                }
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
                auto binding_out = bindings.begin();
                for (const auto& entry : wire)
                {
                    *binding_out = {.binding = entry.binding,
                                    .descriptor_type = entry.descriptor_type,
                                    .descriptor_count = entry.descriptor_count,
                                    .stage_flags = entry.stage_flags};
                    ++binding_out;
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
                auto size_out = sizes.begin();
                for (const auto& entry : wire)
                {
                    *size_out = {.descriptor_type = entry.descriptor_type, .descriptor_count = entry.descriptor_count};
                    ++size_out;
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

            NTSTATUS handle_reset_descriptor_pool(windows_emulator& win_emu, const io_device_context& context)
            {
                gpu_bridge::reset_descriptor_pool_request request{};
                if (!read_input(win_emu, context, request))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                const int32_t result = this->vulkan_.reset_descriptor_pool(request.device, request.descriptor_pool, request.flags);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
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

            // Cap guest-declared descriptor-update payloads so a bogus IOCTL length can't force a huge allocation.
            static constexpr size_t max_descriptor_update_input_bytes = size_t{256} * 1024 * 1024;

            // Applies one update_descriptor_sets_request blob (header + write_count descriptor_write records) and
            // advances `offset` past it. Shared by the single and coalesced-batch IOCTLs.
            int32_t apply_update_descriptor_sets(const std::byte* data, size_t size, size_t& offset)
            {
                constexpr int32_t vk_error_initialization_failed = -3; // VK_ERROR_INITIALIZATION_FAILED (no vulkan.h here)
                using request_t = gpu_bridge::update_descriptor_sets_request;

                if (size - offset < sizeof(request_t))
                {
                    return vk_error_initialization_failed;
                }

                request_t request{};
                std::memcpy(&request, data + offset, sizeof(request));
                offset += sizeof(request);

                const size_t writes_bytes = static_cast<size_t>(request.write_count) * sizeof(gpu_bridge::descriptor_write);
                if (size - offset < writes_bytes)
                {
                    return vk_error_initialization_failed;
                }

                std::vector<vulkan_host::descriptor_write> writes(request.write_count);
                size_t write_offset = offset;
                for (auto& write : writes)
                {
                    gpu_bridge::descriptor_write w{};
                    std::memcpy(&w, data + write_offset, sizeof(w));
                    write_offset += sizeof(w);
                    write = {.dst_set = w.dst_set,
                             .dst_binding = w.dst_binding,
                             .dst_array_element = w.dst_array_element,
                             .descriptor_type = w.descriptor_type,
                             .buffer = w.buffer,
                             .offset = w.offset,
                             .range = w.range,
                             .sampler = w.sampler,
                             .image_view = w.image_view,
                             .image_layout = w.image_layout};
                }
                offset += writes_bytes;

                return this->vulkan_.update_descriptor_sets(request.device, writes);
            }

            NTSTATUS handle_update_descriptor_sets(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length < sizeof(gpu_bridge::update_descriptor_sets_request) ||
                    context.input_buffer_length > max_descriptor_update_input_bytes)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> buffer(context.input_buffer_length);
                win_emu.emu().read_memory(context.input_buffer, buffer.data(), buffer.size());

                size_t offset = 0;
                const int32_t result = this->apply_update_descriptor_sets(buffer.data(), buffer.size(), offset);
                return write_output(win_emu, context, gpu_bridge::result_response{.vk_result = result, .reserved = 0});
            }

            // Applies a concatenation of update_descriptor_sets blobs in order (see update_descriptor_sets_batch).
            NTSTATUS handle_update_descriptor_sets_batch(windows_emulator& win_emu, const io_device_context& context)
            {
                if (!context.input_buffer || context.input_buffer_length == 0 ||
                    context.input_buffer_length > max_descriptor_update_input_bytes)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                std::vector<std::byte> buffer(context.input_buffer_length);
                win_emu.emu().read_memory(context.input_buffer, buffer.data(), buffer.size());

                int32_t result = 0; // VK_SUCCESS
                size_t offset = 0;
                while (offset + sizeof(gpu_bridge::update_descriptor_sets_request) <= buffer.size())
                {
                    const int32_t r = this->apply_update_descriptor_sets(buffer.data(), buffer.size(), offset);
                    if (r != 0 && result == 0)
                    {
                        result = r; // report the first failure
                    }
                    if (r == -3) // malformed record: stop to avoid spinning on a bad offset
                    {
                        break;
                    }
                }

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
                const int32_t result = this->vulkan_.create_sampler(
                    request.device, request.mag_filter, request.min_filter, request.address_mode_u, request.address_mode_v,
                    request.address_mode_w, request.mipmap_mode, request.compare_enable, request.compare_op, request.anisotropy_enable,
                    request.border_color, request.mip_lod_bias, request.max_anisotropy, request.min_lod, request.max_lod, sampler);
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
                    std::vector<uint32_t> color_formats;
                    if (req.is_secondary && req.inherit_color_count > 0)
                    {
                        const size_t formats_bytes = static_cast<size_t>(req.inherit_color_count) * sizeof(uint32_t);
                        if (formats_bytes > size - sizeof(req))
                        {
                            return vk_error_initialization_failed;
                        }
                        color_formats.resize(req.inherit_color_count);
                        std::memcpy(color_formats.data(), payload + sizeof(req), formats_bytes);
                    }
                    return this->vulkan_.begin_command_buffer(req.command_buffer, req.flags, req.is_secondary != 0, req.inherit_view_mask,
                                                              color_formats, req.inherit_depth_format, req.inherit_stencil_format,
                                                              req.inherit_rasterization_samples, req.inherit_rendering_flags);
                }
                case gpu_bridge::command::cmd_execute_commands: {
                    gpu_bridge::cmd_execute_commands_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const size_t ids_bytes = static_cast<size_t>(req.count) * sizeof(gpu_bridge::object_id);
                    if (ids_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<uint64_t> secondaries(req.count);
                    if (req.count > 0)
                    {
                        std::memcpy(secondaries.data(), payload + sizeof(req), ids_bytes);
                    }
                    return this->vulkan_.cmd_execute_commands(req.command_buffer, secondaries);
                }
                case gpu_bridge::command::cmd_set_viewport: {
                    gpu_bridge::cmd_set_viewport_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    static_assert(sizeof(vulkan_host::viewport_entry) == sizeof(gpu_bridge::viewport_entry));
                    const size_t bytes = static_cast<size_t>(req.count) * sizeof(vulkan_host::viewport_entry);
                    if (bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<vulkan_host::viewport_entry> entries(req.count);
                    if (req.count > 0)
                    {
                        std::memcpy(entries.data(), payload + sizeof(req), bytes);
                    }
                    return this->vulkan_.cmd_set_viewport(req.command_buffer, req.first, req.with_count != 0, entries);
                }
                case gpu_bridge::command::cmd_set_scissor: {
                    gpu_bridge::cmd_set_scissor_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    static_assert(sizeof(vulkan_host::scissor_entry) == sizeof(gpu_bridge::scissor_entry));
                    const size_t bytes = static_cast<size_t>(req.count) * sizeof(vulkan_host::scissor_entry);
                    if (bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    std::vector<vulkan_host::scissor_entry> entries(req.count);
                    if (req.count > 0)
                    {
                        std::memcpy(entries.data(), payload + sizeof(req), bytes);
                    }
                    return this->vulkan_.cmd_set_scissor(req.command_buffer, req.first, req.with_count != 0, entries);
                }
                case gpu_bridge::command::cmd_set_depth_bias: {
                    gpu_bridge::cmd_set_depth_bias_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_depth_bias(req.command_buffer, req.constant_factor, req.clamp, req.slope_factor);
                }
                case gpu_bridge::command::cmd_set_blend_constants: {
                    gpu_bridge::cmd_set_blend_constants_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_blend_constants(req.command_buffer, req.constants);
                }
                case gpu_bridge::command::cmd_set_depth_bounds: {
                    gpu_bridge::cmd_set_depth_bounds_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_depth_bounds(req.command_buffer, req.min_depth_bounds, req.max_depth_bounds);
                }
                case gpu_bridge::command::cmd_set_line_width: {
                    gpu_bridge::cmd_set_line_width_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_line_width(req.command_buffer, req.line_width);
                }
                case gpu_bridge::command::cmd_set_stencil: {
                    gpu_bridge::cmd_set_stencil_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_stencil(req.command_buffer, req.which, req.face_mask, req.value);
                }
                case gpu_bridge::command::cmd_set_stencil_op: {
                    gpu_bridge::cmd_set_stencil_op_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_stencil_op(req.command_buffer, req.face_mask, req.fail_op, req.pass_op, req.depth_fail_op,
                                                            req.compare_op);
                }
                case gpu_bridge::command::cmd_set_dynamic_u32: {
                    gpu_bridge::cmd_set_dynamic_u32_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_set_dynamic_u32(req.command_buffer, req.state, req.value);
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
                    size_t region_offset = sizeof(req);
                    for (auto& region : regions)
                    {
                        gpu_bridge::buffer_copy_region r{};
                        std::memcpy(&r, payload + region_offset, sizeof(r));
                        region_offset += sizeof(r);
                        region = vulkan_host::buffer_copy{.src_offset = r.src_offset, .dst_offset = r.dst_offset, .size = r.size};
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
                    auto buffer_id_out = buffer_ids.begin();
                    auto offset_out = offsets.begin();
                    size_t binding_offset = sizeof(req);
                    for (uint32_t i = 0; i < req.binding_count; ++i)
                    {
                        gpu_bridge::vertex_buffer_binding vb{};
                        std::memcpy(&vb, payload + binding_offset, sizeof(vb));
                        binding_offset += sizeof(vb);
                        *buffer_id_out = vb.buffer;
                        *offset_out = vb.offset;
                        ++buffer_id_out;
                        ++offset_out;
                    }
                    return this->vulkan_.cmd_bind_vertex_buffers(req.command_buffer, req.first_binding, req.binding_count,
                                                                 buffer_ids.data(), offsets.data());
                }
                case gpu_bridge::command::cmd_bind_vertex_buffers2: {
                    gpu_bridge::cmd_bind_vertex_buffers2_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const size_t bindings_bytes = static_cast<size_t>(req.binding_count) * sizeof(gpu_bridge::vertex_buffer_binding2);
                    if (bindings_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    // Reused scratch (single emulator thread, no reentrancy) - this path runs ~40k times/s
                    // in heavy scenes, so a per-call heap allocation here is pure overhead.
                    static thread_local std::vector<uint64_t> buffer_ids;
                    static thread_local std::vector<uint64_t> offsets;
                    static thread_local std::vector<uint64_t> sizes;
                    static thread_local std::vector<uint64_t> strides;
                    buffer_ids.resize(req.binding_count);
                    offsets.resize(req.binding_count);
                    sizes.resize(req.binding_count);
                    strides.resize(req.binding_count);
                    auto buffer_id_out = buffer_ids.begin();
                    auto offset_out = offsets.begin();
                    auto size_out = sizes.begin();
                    auto stride_out = strides.begin();
                    size_t binding_offset = sizeof(req);
                    for (uint32_t i = 0; i < req.binding_count; ++i)
                    {
                        gpu_bridge::vertex_buffer_binding2 vb{};
                        std::memcpy(&vb, payload + binding_offset, sizeof(vb));
                        binding_offset += sizeof(vb);
                        *buffer_id_out = vb.buffer;
                        *offset_out = vb.offset;
                        *size_out = vb.size;
                        *stride_out = vb.stride;
                        ++buffer_id_out;
                        ++offset_out;
                        ++size_out;
                        ++stride_out;
                    }
                    return this->vulkan_.cmd_bind_vertex_buffers2(req.command_buffer, req.first_binding, req.binding_count,
                                                                  buffer_ids.data(), offsets.data(), req.has_sizes ? sizes.data() : nullptr,
                                                                  req.has_strides ? strides.data() : nullptr);
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
                    const size_t offsets_bytes = static_cast<size_t>(req.dynamic_offset_count) * sizeof(uint32_t);
                    if (ids_bytes + offsets_bytes > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    // Reused scratch (single emulator thread, no reentrancy) - this path runs ~100k times/s
                    // in heavy scenes, so a per-call heap allocation here is pure overhead.
                    static thread_local std::vector<uint64_t> sets;
                    static thread_local std::vector<uint32_t> dynamic_offsets;
                    sets.resize(req.set_count);
                    if (req.set_count > 0)
                    {
                        std::memcpy(sets.data(), payload + sizeof(req), ids_bytes);
                    }
                    dynamic_offsets.resize(req.dynamic_offset_count);
                    if (req.dynamic_offset_count > 0)
                    {
                        std::memcpy(dynamic_offsets.data(), payload + sizeof(req) + ids_bytes, offsets_bytes);
                    }
                    return this->vulkan_.cmd_bind_descriptor_sets(req.command_buffer, req.pipeline_layout, req.first_set, sets,
                                                                  req.bind_point, dynamic_offsets);
                }
                case gpu_bridge::command::cmd_end_render_pass: {
                    gpu_bridge::cmd_end_render_pass_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_end_render_pass(req.command_buffer);
                }
                case gpu_bridge::command::cmd_begin_rendering: {
                    gpu_bridge::cmd_begin_rendering_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const uint32_t total = req.color_attachment_count + (req.has_depth ? 1u : 0u) + (req.has_stencil ? 1u : 0u);
                    if (static_cast<size_t>(total) * sizeof(gpu_bridge::rendering_attachment) > size - sizeof(req))
                    {
                        return vk_error_initialization_failed;
                    }

                    const auto convert = [&](size_t index) {
                        gpu_bridge::rendering_attachment w{};
                        std::memcpy(&w, payload + sizeof(req) + index * sizeof(w), sizeof(w));
                        vulkan_host::rendering_attachment a{};
                        a.image_view = w.image_view;
                        a.resolve_image_view = w.resolve_image_view;
                        a.image_layout = w.image_layout;
                        a.resolve_image_layout = w.resolve_image_layout;
                        a.resolve_mode = w.resolve_mode;
                        a.load_op = w.load_op;
                        a.store_op = w.store_op;
                        std::memcpy(a.clear_value.data(), w.clear_value.data(), sizeof(a.clear_value));
                        return a;
                    };

                    size_t next = 0;
                    std::vector<vulkan_host::rendering_attachment> color(req.color_attachment_count);
                    for (auto& attachment : color)
                    {
                        attachment = convert(next++);
                    }
                    vulkan_host::rendering_attachment depth{};
                    vulkan_host::rendering_attachment stencil{};
                    if (req.has_depth)
                    {
                        depth = convert(next++);
                    }
                    if (req.has_stencil)
                    {
                        stencil = convert(next++);
                    }

                    return this->vulkan_.cmd_begin_rendering(req.command_buffer, req.render_area_x, req.render_area_y,
                                                             req.render_area_width, req.render_area_height, req.layer_count, req.view_mask,
                                                             req.flags, color, req.has_depth ? &depth : nullptr,
                                                             req.has_stencil ? &stencil : nullptr);
                }
                case gpu_bridge::command::cmd_end_rendering: {
                    gpu_bridge::cmd_end_rendering_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_end_rendering(req.command_buffer);
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
                case gpu_bridge::command::cmd_clear_attachments: {
                    gpu_bridge::cmd_clear_attachments_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_clear_attachments(req.command_buffer, req.attachment_count, req.rect_count,
                                                               payload + sizeof(req), size - sizeof(req));
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
                    const vulkan_host::buffer_image_copy_region region{
                        .buffer_offset = req.buffer_offset,
                        .buffer_row_length = req.buffer_row_length,
                        .buffer_image_height = req.buffer_image_height,
                        .image_offset_x = req.image_offset_x,
                        .image_offset_y = req.image_offset_y,
                        .image_offset_z = req.image_offset_z,
                        .width = req.width,
                        .height = req.height,
                        .depth = req.depth,
                        .mip_level = req.mip_level,
                        .base_array_layer = req.base_array_layer,
                        .layer_count = req.layer_count,
                        .aspect_mask = req.aspect_mask,
                    };
                    return this->vulkan_.cmd_copy_buffer_to_image(req.command_buffer, req.buffer, req.image, req.image_layout, region);
                }
                case gpu_bridge::command::cmd_resolve_image: {
                    gpu_bridge::cmd_resolve_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    return this->vulkan_.cmd_resolve_image(req.command_buffer, req.src_image, req.src_layout, req.dst_image, req.dst_layout,
                                                           req.width, req.height, req.aspect_mask);
                }
                case gpu_bridge::command::cmd_copy_image: {
                    gpu_bridge::cmd_copy_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const vulkan_host::image_copy_region region{
                        .src_aspect_mask = req.src_aspect_mask,
                        .src_mip_level = req.src_mip_level,
                        .src_base_array_layer = req.src_base_array_layer,
                        .src_layer_count = req.src_layer_count,
                        .src_offset_x = req.src_offset_x,
                        .src_offset_y = req.src_offset_y,
                        .src_offset_z = req.src_offset_z,
                        .dst_aspect_mask = req.dst_aspect_mask,
                        .dst_mip_level = req.dst_mip_level,
                        .dst_base_array_layer = req.dst_base_array_layer,
                        .dst_layer_count = req.dst_layer_count,
                        .dst_offset_x = req.dst_offset_x,
                        .dst_offset_y = req.dst_offset_y,
                        .dst_offset_z = req.dst_offset_z,
                        .width = req.width,
                        .height = req.height,
                        .depth = req.depth,
                    };
                    return this->vulkan_.cmd_copy_image(req.command_buffer, req.src_image, req.src_layout, req.dst_image, req.dst_layout,
                                                        region);
                }
                case gpu_bridge::command::cmd_blit_image: {
                    gpu_bridge::cmd_blit_image_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const vulkan_host::image_blit_region region{
                        .src_aspect_mask = req.src_aspect_mask,
                        .src_mip_level = req.src_mip_level,
                        .src_base_array_layer = req.src_base_array_layer,
                        .src_layer_count = req.src_layer_count,
                        .src_offset_x0 = req.src_offset_x0,
                        .src_offset_y0 = req.src_offset_y0,
                        .src_offset_z0 = req.src_offset_z0,
                        .src_offset_x1 = req.src_offset_x1,
                        .src_offset_y1 = req.src_offset_y1,
                        .src_offset_z1 = req.src_offset_z1,
                        .dst_aspect_mask = req.dst_aspect_mask,
                        .dst_mip_level = req.dst_mip_level,
                        .dst_base_array_layer = req.dst_base_array_layer,
                        .dst_layer_count = req.dst_layer_count,
                        .dst_offset_x0 = req.dst_offset_x0,
                        .dst_offset_y0 = req.dst_offset_y0,
                        .dst_offset_z0 = req.dst_offset_z0,
                        .dst_offset_x1 = req.dst_offset_x1,
                        .dst_offset_y1 = req.dst_offset_y1,
                        .dst_offset_z1 = req.dst_offset_z1,
                        .filter = req.filter,
                    };
                    return this->vulkan_.cmd_blit_image(req.command_buffer, req.src_image, req.src_layout, req.dst_image, req.dst_layout,
                                                        region);
                }
                case gpu_bridge::command::cmd_update_buffer: {
                    gpu_bridge::cmd_update_buffer_request req{};
                    if (!read(req))
                    {
                        return vk_error_initialization_failed;
                    }
                    const auto data_bytes = std::min<uint64_t>(req.size, size - sizeof(req));
                    return this->vulkan_.cmd_update_buffer(req.command_buffer, req.buffer, req.offset, payload + sizeof(req),
                                                           static_cast<uint32_t>(data_bytes));
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

                // Resolve the guest window's client size so the host can report it as the surface's current
                // extent; otherwise the layer (DXVK) can't determine the size and creates a 0x0 swapchain.
                uint32_t window_width = 0;
                uint32_t window_height = 0;
                if (const uint64_t surface_hwnd = this->vulkan_.get_surface_hwnd(request.surface); surface_hwnd != 0)
                {
                    if (const auto* win = win_emu.process.windows.get(static_cast<hwnd>(surface_hwnd));
                        win && win->client_width() > 0 && win->client_height() > 0)
                    {
                        // Client size, not outer: DXVK pins its swapchain to this extent and must match its
                        // client-sized backbuffer, else the present upscales (see window::nonclient_border).
                        window_width = static_cast<uint32_t>(win->client_width());
                        window_height = static_cast<uint32_t>(win->client_height());
                    }
                }

                std::vector<std::byte> caps(context.output_buffer_length);
                const int32_t result = this->vulkan_.get_surface_capabilities(request.physical_device, request.surface, window_width,
                                                                              window_height, caps.data(), caps.size());
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
