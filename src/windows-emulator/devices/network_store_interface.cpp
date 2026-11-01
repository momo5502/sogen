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

        // GetAllParameters and EnumerateObjectsAllParameters pass four buffers: the object key and its read-write,
        // dynamic and static parameters. Pointers are native words, sizes are ULONGs, so under WoW64 the descriptors
        // are packed tighter.
        struct nsi_all_parameters
        {
            nsi_buffer key;
            nsi_buffer rw;
            nsi_buffer dynamic;
            nsi_buffer static_parameters;
        };

        uint64_t read_pointer(windows_emulator& win_emu, const io_device_context& c, const ULONG offset, const bool wow64)
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

        uint64_t read_size(windows_emulator& win_emu, const io_device_context& c, const ULONG offset)
        {
            if (c.input_buffer_length < offset + sizeof(ULONG))
            {
                return 0;
            }

            return win_emu.emu().read_memory<ULONG>(c.input_buffer + offset);
        }

        nsi_buffer read_buffer(windows_emulator& win_emu, const io_device_context& c, const ULONG offset, const bool wow64)
        {
            const ULONG pointer_size = wow64 ? sizeof(uint32_t) : sizeof(uint64_t);
            return nsi_buffer{.address = read_pointer(win_emu, c, offset, wow64), .size = read_size(win_emu, c, offset + pointer_size)};
        }

        nsi_all_parameters read_all_parameters(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
        {
            const ULONG first = wow64 ? 0x18 : 0x28;
            const ULONG stride = wow64 ? 0x8 : 0x10;
            return nsi_all_parameters{
                .key = read_buffer(win_emu, c, first, wow64),
                .rw = read_buffer(win_emu, c, first + stride, wow64),
                .dynamic = read_buffer(win_emu, c, first + (2 * stride), wow64),
                .static_parameters = read_buffer(win_emu, c, first + (3 * stride), wow64),
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

            win_emu.emu().set_memory(buffer.address, 0, static_cast<size_t>(buffer.size));
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

        void write_adapter_object(windows_emulator& win_emu, const nsi_all_parameters& parameters)
        {
            write_adapter_key(win_emu, parameters.key);
            write_zeroes(win_emu, parameters.rw);
            write_zeroes(win_emu, parameters.dynamic);
            write_adapter_parameter(win_emu, parameters.dynamic);
            write_zeroes(win_emu, parameters.static_parameters);
        }

        struct network_store_interface_device : stateless_device
        {
            NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
            {
                const bool wow64 = win_emu.process.is_wow64_process;
                switch (c.io_control_code)
                {
                case k_nsi_get_parameter:
                    return get_parameter(win_emu, c, wow64);
                case k_nsi_get_all_parameters:
                    write_adapter_object(win_emu, read_all_parameters(win_emu, c, wow64));
                    write_io_information(c, 0);
                    return STATUS_SUCCESS;
                case k_nsi_enumerate_objects_all_parameters:
                    return enumerate_objects_all_parameters(win_emu, c, wow64);
                default:
                    return STATUS_SUCCESS;
                }
            }

            // GetParameter names one parameter: after the key comes the parameter type, then the output buffer, whose
            // ULONG size is followed by the parameter's offset inside its structure.
            static NTSTATUS get_parameter(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
            {
                const ULONG data_offset = wow64 ? 0x24 : 0x40;
                const auto data = read_buffer(win_emu, c, data_offset, wow64);
                const nsi_buffer output{.address = data.address ? data.address : c.output_buffer,
                                        .size = data.address ? data.size : c.output_buffer_length};

                if (output.size == sizeof(uint32_t))
                {
                    if (output.address)
                    {
                        win_emu.emu().write_memory<uint32_t>(output.address, 1);
                    }
                }
                else if (output.size == sizeof(uint64_t))
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

            static NTSTATUS enumerate_objects_all_parameters(windows_emulator& win_emu, const io_device_context& c, const bool wow64)
            {
                // Report a single enumerated object by writing the count field that follows the buffer descriptors.
                const ULONG count_offset = wow64 ? 0x38 : 0x68;
                if (c.input_buffer_length >= count_offset + sizeof(ULONG))
                {
                    win_emu.emu().write_memory<ULONG>(c.input_buffer + count_offset, 1);
                }

                write_adapter_object(win_emu, read_all_parameters(win_emu, c, wow64));
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
