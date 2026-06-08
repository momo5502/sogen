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
        };
    }

    std::unique_ptr<io_device> create_gpu_bridge()
    {
        return std::make_unique<gpu_bridge_device>();
    }
}
