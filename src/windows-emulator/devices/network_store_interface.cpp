#include "../std_include.hpp"
#include "network_store_interface.hpp"

#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        constexpr ULONG k_nsi_get_parameter = 0x120007;
        constexpr ULONG k_nsi_get_all_parameters = 0x12000F;
        constexpr ULONG k_nsi_enumerate_objects_all_parameters = 0x12001B;
        constexpr uint64_t k_adapter_index = 7;

        constexpr std::array<uint8_t, 0xB8> k_adapter_parameter = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x30, 0x75, 0x00, 0x00, 0xE8, 0x03, 0x00, 0x00, 0xC0, 0x27,
            0x09, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x19, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00,
            0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xDC, 0x05, 0x00, 0x00, 0x40, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x35, 0x97, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00,
        };

        struct nsi_buffer
        {
            uint64_t address{};
            uint64_t size{};
        };

        struct nsi_request
        {
            nsi_buffer key;
            nsi_buffer rw;
            nsi_buffer dynamic;
        };

        // The NSI request structure stores pointers and lengths as native words, so under WoW64 it is
        // packed with 4-byte fields at different offsets than the 64-bit layout.
        struct nsi_offsets
        {
            ULONG key_address;
            ULONG key_size;
            ULONG rw_address;
            ULONG rw_size;
            ULONG dynamic_address;
            ULONG dynamic_size;
        };

        uint64_t read_field(windows_emulator& win_emu, const io_device_context& c, const ULONG offset, const bool wow64)
        {
            const ULONG width = wow64 ? sizeof(uint32_t) : sizeof(uint64_t);
            if (c.input_buffer_length < offset + width)
            {
                return 0;
            }

            if (wow64)
            {
                return win_emu.emu().read_memory<uint32_t>(c.input_buffer + offset);
            }

            return win_emu.emu().read_memory<uint64_t>(c.input_buffer + offset);
        }

        nsi_buffer read_buffer(windows_emulator& win_emu, const io_device_context& c, const ULONG address_offset, const ULONG size_offset,
                               const bool wow64)
        {
            return nsi_buffer{.address = read_field(win_emu, c, address_offset, wow64), .size = read_field(win_emu, c, size_offset, wow64)};
        }

        nsi_request read_request(windows_emulator& win_emu, const io_device_context& c, const nsi_offsets& offsets, const bool wow64)
        {
            return nsi_request{
                .key = read_buffer(win_emu, c, offsets.key_address, offsets.key_size, wow64),
                .rw = read_buffer(win_emu, c, offsets.rw_address, offsets.rw_size, wow64),
                .dynamic = read_buffer(win_emu, c, offsets.dynamic_address, offsets.dynamic_size, wow64),
            };
        }

        void write_io_information(const io_device_context& c, const uint64_t information)
        {
            if (!c.io_status_block)
            {
                return;
            }

            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = information;
            c.io_status_block.write(block);
        }

        void write_zeroes(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || !buffer.size)
            {
                return;
            }

            win_emu.emu().set_memory(buffer.address, 0, buffer.size);
        }

        void write_adapter_key(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || buffer.size < sizeof(uint64_t))
            {
                return;
            }

            win_emu.emu().write_memory<uint64_t>(buffer.address, k_adapter_index);
        }

        void write_adapter_parameter(windows_emulator& win_emu, const nsi_buffer buffer)
        {
            if (!buffer.address || !buffer.size)
            {
                return;
            }

            const auto bytes_to_write = std::min<size_t>(static_cast<size_t>(buffer.size), k_adapter_parameter.size());
            win_emu.emu().write_memory(buffer.address, k_adapter_parameter.data(), bytes_to_write);
        }

        struct network_store_interface_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                const bool wow64 = win_emu.process.is_wow64_process;
                switch (c.io_control_code)
                {
                case k_nsi_get_parameter:
                    return get_parameter(win_emu, c, read_request(win_emu, c, get_offsets(wow64), wow64));
                case k_nsi_get_all_parameters:
                    return get_all_parameters(win_emu, c, read_request(win_emu, c, get_offsets(wow64), wow64));
                case k_nsi_enumerate_objects_all_parameters:
                    return enumerate_objects_all_parameters(win_emu, c, read_request(win_emu, c, enumerate_offsets(wow64), wow64), wow64);
                default:
                    return STATUS_SUCCESS;
                }
            }

            static nsi_offsets get_offsets(const bool wow64)
            {
                return wow64 ? nsi_offsets{.key_address = 0x18,
                                           .key_size = 0x1C,
                                           .rw_address = 0x24,
                                           .rw_size = 0x28,
                                           .dynamic_address = 0x2C,
                                           .dynamic_size = 0x30}
                             : nsi_offsets{.key_address = 0x28,
                                           .key_size = 0x30,
                                           .rw_address = 0x40,
                                           .rw_size = 0x48,
                                           .dynamic_address = 0x50,
                                           .dynamic_size = 0x58};
            }

            static nsi_offsets enumerate_offsets(const bool wow64)
            {
                return wow64 ? nsi_offsets{.key_address = 0x18,
                                           .key_size = 0x1C,
                                           .rw_address = 0x28,
                                           .rw_size = 0x2C,
                                           .dynamic_address = 0x30,
                                           .dynamic_size = 0x34}
                             : nsi_offsets{.key_address = 0x28,
                                           .key_size = 0x30,
                                           .rw_address = 0x48,
                                           .rw_size = 0x50,
                                           .dynamic_address = 0x58,
                                           .dynamic_size = 0x60};
            }

            static NTSTATUS get_parameter(windows_emulator& win_emu, const io_device_context& c, const nsi_request& request)
            {
                const nsi_buffer output{.address = request.rw.address ? request.rw.address : c.output_buffer,
                                        .size = c.output_buffer_length};

                if (c.output_buffer_length == sizeof(uint32_t))
                {
                    if (output.address)
                    {
                        win_emu.emu().write_memory<uint32_t>(output.address, 1);
                    }
                }
                else if (c.output_buffer_length == sizeof(uint64_t))
                {
                    if (output.address)
                    {
                        win_emu.emu().write_memory<uint64_t>(output.address, k_adapter_index);
                    }
                }
                else
                {
                    write_adapter_parameter(win_emu, output);
                }

                write_io_information(c, 0);
                return STATUS_SUCCESS;
            }

            static NTSTATUS get_all_parameters(windows_emulator& win_emu, const io_device_context& c, const nsi_request& request)
            {
                write_adapter_key(win_emu, request.key);
                write_adapter_parameter(win_emu, request.rw);
                write_zeroes(win_emu, request.dynamic);
                write_io_information(c, 0);
                return STATUS_SUCCESS;
            }

            static NTSTATUS enumerate_objects_all_parameters(windows_emulator& win_emu, const io_device_context& c,
                                                             const nsi_request& request, const bool wow64)
            {
                // Report a single enumerated object by writing the count field that follows the buffer descriptors.
                if (wow64)
                {
                    if (c.input_buffer_length >= 0x3C)
                    {
                        win_emu.emu().write_memory<uint32_t>(c.input_buffer + 0x38, 1);
                    }
                }
                else if (c.input_buffer_length >= 0x70)
                {
                    win_emu.emu().write_memory<uint64_t>(c.input_buffer + 0x68, 1);
                }

                write_adapter_key(win_emu, request.key);
                write_adapter_parameter(win_emu, request.rw);
                write_zeroes(win_emu, request.dynamic);
                write_io_information(c, 0);
                return STATUS_SUCCESS;
            }
        };
    }

    std::unique_ptr<io_device> create_network_store_interface(const device_creation_context&)
    {
        return std::make_unique<network_store_interface_device>();
    }
}
