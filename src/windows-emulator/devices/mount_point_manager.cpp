#include "../std_include.hpp"
#include "mount_point_manager.hpp"
#include <charconv>
#include <cstring>

#include "../windows_emulator.hpp"
#include "mountmgr.hpp"

namespace sogen
{

    namespace
    {
        struct mountdev_target_name_header
        {
            USHORT device_name_length;
        };

        std::pair<ULONG, USHORT> write_data(std::vector<uint8_t>& buffer, const std::span<const uint8_t> data)
        {
            const auto offset = buffer.size();
            buffer.insert(buffer.end(), data.begin(), data.end());
            return std::make_pair(static_cast<ULONG>(offset), static_cast<USHORT>(data.size()));
        }

        template <typename Char>
        std::pair<ULONG, USHORT> write_string(std::vector<uint8_t>& buffer, const std::basic_string_view<Char> str)
        {
            std::span data(reinterpret_cast<const uint8_t*>(str.data()), str.size() * sizeof(Char));
            return write_data(buffer, data);
        }

        std::string make_drive_id(const uint64_t low = 0, const uint64_t high = 0)
        {
            std::string id = "DMIO:ID:";
            id.append(reinterpret_cast<const char*>(&low), sizeof(low));
            id.append(reinterpret_cast<const char*>(&high), sizeof(high));

            return id;
        }

        std::u16string make_volume(const uint64_t low = 0, const uint64_t high = 0)
        {
            const auto raw = utils::string::to_hex_string(low) + utils::string::to_hex_string(high);
            const std::string_view raw_view{raw};
            std::string str{};
            str.reserve(36);
            str.append(raw_view.substr(0, 8));
            str.push_back('-');
            str.append(raw_view.substr(8, 4));
            str.push_back('-');
            str.append(raw_view.substr(12, 4));
            str.push_back('-');
            str.append(raw_view.substr(16, 4));
            str.push_back('-');
            str.append(raw_view.substr(20, 12));

            const std::string volume = utils::string::va("\\??\\Volume{%s}", str.c_str());
            return u8_to_u16(volume);
        }

        struct mount_point_manager : stateless_device
        {
            static NTSTATUS query_points(windows_emulator& win_emu, const io_device_context& c)
            {
                const auto drives = win_emu.file_sys.list_drives();
                const auto struct_size = sizeof(MOUNTMGR_MOUNT_POINTS) + sizeof(MOUNTMGR_MOUNT_POINT) * drives.size();

                std::vector<MOUNTMGR_MOUNT_POINT> mount_points{};

                std::vector<uint8_t> buffer{};
                buffer.resize(struct_size);

                {
                    MOUNTMGR_MOUNT_POINT point{};
                    const auto symlink = write_string<char16_t>(buffer, u"\\DosDevices\\");
                    const auto id = write_string<char>(buffer, make_drive_id(0, 1));
                    const auto name = write_string<char16_t>(buffer, u"\\Device\\HarddiskVolume0");

                    point.SymbolicLinkNameOffset = symlink.first;
                    point.SymbolicLinkNameLength = symlink.second;

                    point.UniqueIdOffset = id.first;
                    point.UniqueIdLength = id.second;

                    point.DeviceNameOffset = name.first;
                    point.DeviceNameLength = name.second;

                    mount_points.push_back(point);
                }

                for (const auto drive : drives)
                {
                    MOUNTMGR_MOUNT_POINT point{};
                    const auto symlink = write_string<char16_t>(buffer, make_volume(drive, 0));
                    const auto id = write_string<char>(buffer, make_drive_id(drive, 0));
                    const auto name =
                        write_string<char16_t>(buffer, u"\\Device\\HarddiskVolume" + u8_to_u16(std::to_string(drive - 'a' + 1)));

                    point.SymbolicLinkNameOffset = symlink.first;
                    point.SymbolicLinkNameLength = symlink.second;

                    point.UniqueIdOffset = id.first;
                    point.UniqueIdLength = id.second;

                    point.DeviceNameOffset = name.first;
                    point.DeviceNameLength = name.second;

                    mount_points.push_back(point);
                }

                MOUNTMGR_MOUNT_POINTS points{};
                points.Size = static_cast<ULONG>(buffer.size());
                points.NumberOfMountPoints = static_cast<ULONG>(mount_points.size());

                memcpy(buffer.data(), &points, sizeof(points));
                memcpy(buffer.data() + offsetof(MOUNTMGR_MOUNT_POINTS, MountPoints), mount_points.data(),
                       mount_points.size() * sizeof(MOUNTMGR_MOUNT_POINT));

                const auto length = std::min(static_cast<size_t>(c.output_buffer_length), buffer.size());

                win_emu.emu().write_memory(c.output_buffer, buffer.data(), length);

                if (c.io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = buffer.size();
                    c.io_status_block.write(block);
                }

                return length < buffer.size() ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
            }

            static NTSTATUS get_drive_letter(windows_emulator& win_emu, const io_device_context& c)
            {
                if (c.input_buffer_length < sizeof(mountdev_target_name_header))
                {
                    return STATUS_NOT_SUPPORTED;
                }

                // input_buffer_length is guest-controlled (up to 4 GiB). A device name is tiny, so
                // cap the buffer we read to avoid a huge host allocation.
                if (c.input_buffer_length > 0x1000)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                const auto data = win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);
                mountdev_target_name_header target_name{};
                std::memcpy(&target_name, data.data(), sizeof(target_name));

                constexpr auto name_offset = sizeof(mountdev_target_name_header);
                const auto name_length = static_cast<size_t>(target_name.device_name_length);
                if (name_length == 0 || name_length % sizeof(char16_t) != 0 || data.size() - name_offset < name_length)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                const std::u16string_view file(reinterpret_cast<const char16_t*>(data.data() + name_offset),
                                               name_length / sizeof(char16_t));

                constexpr std::u16string_view volume_prefix = u"\\Device\\HarddiskVolume";
                if (!file.starts_with(volume_prefix))
                {
                    return STATUS_NOT_SUPPORTED;
                }

                const auto drive_number = file.substr(volume_prefix.size());
                const auto drive_number_u8 = u16_to_u8(drive_number);
                int drive_index{};
                const auto* number_start = drive_number_u8.data();
                const auto* number_end = number_start + drive_number_u8.size();
                const auto [parse_end, parse_error] = std::from_chars(number_start, number_end, drive_index);
                if (parse_error != std::errc{} || parse_end != number_end || drive_index < 1 || drive_index > 26)
                {
                    return STATUS_NOT_SUPPORTED;
                }

                const auto drive_letter = static_cast<char>('A' + drive_index - 1);

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

            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                if (c.io_control_code == IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH)
                {
                    return get_drive_letter(win_emu, c);
                }

                if (c.io_control_code == IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATHS)
                {
                    return get_drive_letter(win_emu, c);
                }

                if (c.io_control_code == IOCTL_MOUNTMGR_QUERY_POINTS)
                {
                    return query_points(win_emu, c);
                }

                win_emu.log.error("Unsupported mount point IOCTL: %X\n", static_cast<uint32_t>(c.io_control_code));
                return STATUS_NOT_SUPPORTED;
            }
        };
    }

    std::unique_ptr<io_device> create_mount_point_manager(const device_creation_context&)
    {
        return std::make_unique<mount_point_manager>();
    }

} // namespace sogen
