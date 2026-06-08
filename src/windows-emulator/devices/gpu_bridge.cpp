#include "../std_include.hpp"
#include "gpu_bridge.hpp"
#include "../windows_emulator.hpp"

#include <gpu_bridge_protocol.hpp>

namespace sogen
{
    namespace
    {
        // Host endpoint of the GPU paravirtualization bridge. The guest reaches it by opening
        // \\.\SogenGpu and issuing IOCTLs; each control code maps to one bridge command, with the
        // payload carried in the input/output buffers. For now it answers the protocol handshake;
        // the Vulkan object model and command marshalling will be layered onto io_control later.
        struct gpu_bridge_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
            {
                switch (context.io_control_code)
                {
                case gpu_bridge::ioctl_get_version:
                    return handle_get_version(win_emu, context);

                default:
                    win_emu.log.warn("[gpu-bridge] Unsupported IOCTL: 0x%X\n", context.io_control_code);
                    return STATUS_NOT_SUPPORTED;
                }
            }

          private:
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

                if (context.io_status_block)
                {
                    context.io_status_block.access(
                        [&](IO_STATUS_BLOCK<EmulatorTraits<Emu64>>& block) { block.Information = response_size; });
                }

                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_gpu_bridge()
    {
        return std::make_unique<gpu_bridge_device>();
    }
}
