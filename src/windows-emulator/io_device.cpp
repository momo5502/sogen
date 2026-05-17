#include "std_include.hpp"
#include "io_device.hpp"
#include "windows_emulator.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"
#include "devices/named_pipe.hpp"
#include <iostream>

namespace
{
    struct dummy_device : stateless_device
    {
        NTSTATUS io_control(windows_emulator&, const io_device_context&) override
        {
            return STATUS_SUCCESS;
        }
    };

    struct transport_stub_device : stateless_device
    {
        NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& context) override
        {
            if (context.output_buffer && context.output_buffer_length)
            {
                std::vector<std::byte> output(context.output_buffer_length, std::byte{0});
                win_emu.emu().write_memory(context.output_buffer, output.data(), output.size());
            }

            if (context.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = context.output_buffer_length;
                context.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }
    };
}

bool needs_32_bit_devices(const windows_emulator& win_emu)
{
    return win_emu.process.is_wow64_process;
}

std::unique_ptr<io_device> create_device(const std::u16string_view device, const bool is_32_bit)
{
    if (device == u"CNG"                    //
        || device == u"Nsi"                 //
        || device == u"RasAcd"              //
        || device == u"PcwDrv"              //
        || device == u"DeviceApi\\CMApi"    //
        || device == u"DeviceApi\\CMNotify" //
        || device == u"ConDrv\\Server")
    {
        return std::make_unique<dummy_device>();
    }

    if (device == u"Afd\\Endpoint")
    {
        return create_afd_endpoint(is_32_bit);
    }

    if (device == u"Afd\\AsyncConnectHlp")
    {
        return create_afd_async_connect_hlp(is_32_bit);
    }

    if (device == u"MountPointManager")
    {
        return create_mount_point_manager();
    }

    if (device == u"KsecDD")
    {
        return create_security_support_provider();
    }

    if (device == u"NamedPipe")
    {
        return std::make_unique<named_pipe>();
    }

    if (device == u"Tcp" || device == u"Tcp6" || device == u"Udp" || device == u"RawIp")
    {
        return std::make_unique<transport_stub_device>();
    }

    throw std::runtime_error("Unsupported device: " + u16_to_u8(device));
}

NTSTATUS io_device_container::io_control(windows_emulator& win_emu, const io_device_context& context)
{
    this->assert_validity();
    win_emu.callbacks.on_ioctrl(*this->device_, this->device_name_, context.io_control_code);
    return this->device_->io_control(win_emu, context);
}

void io_device_container::work(windows_emulator& win_emu)
{
    this->assert_validity();
    this->device_->work(win_emu);
}

void io_device_container::serialize_object(utils::buffer_serializer& buffer) const
{
    this->assert_validity();

    buffer.write(this->is_32_bit_);
    buffer.write_string(this->device_name_);
    this->device_->serialize(buffer);
}

void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
{
    buffer.read(this->is_32_bit_);
    buffer.read_string(this->device_name_);

    this->setup();
    this->device_->deserialize(buffer);
}
