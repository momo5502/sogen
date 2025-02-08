#include "mount_point_manager.hpp"

#include "../windows_emulator.hpp"

namespace
{
    struct mount_point_manager : stateless_device
    {
        NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
        {
            if (c.io_control_code != 0x6D0030)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (c.input_buffer_length < 2)
            {
                return STATUS_NOT_SUPPORTED;
            }

            const auto data = win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);

            const std::u16string_view file(reinterpret_cast<const char16_t*>(data.data()), (data.size() / 2) - 1);

            constexpr std::u16string_view volume_prefix = u".\\Device\\HarddiskVolume";
            if (!file.starts_with(volume_prefix))
            {
                return STATUS_NOT_SUPPORTED;
            }

            const auto drive_number = file.substr(volume_prefix.size());
            const auto drive_number_u8 = u16_to_u8(drive_number);
            const auto drive_letter = static_cast<char>('A' + atoi(drive_number_u8.c_str()) - 1);

            std::string response{};
            response.push_back(drive_letter);
            response.push_back(':');
            response.push_back(0);
            response.push_back(0);

            const auto u16_response = u8_to_u16(response);

            const auto length = static_cast<uint32_t>(u16_response.size() * 2);
            const auto total_length = sizeof(length) + length;

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = total_length;
                c.io_status_block.write(block);
            }

            if (c.output_buffer_length < total_length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            win_emu.emu().write_memory(c.output_buffer, length);
            win_emu.emu().write_memory(c.output_buffer + sizeof(length), u16_response.data(), length);

            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<io_device> create_mount_point_manager()
{
    return std::make_unique<mount_point_manager>();
}
