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
}

std::unique_ptr<io_device> create_device(const std::u16string_view device)
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
        return create_afd_endpoint();
    }

    if (device == u"Afd\\AsyncConnectHlp")
    {
        return create_afd_async_connect_hlp();
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

    buffer.write_string(this->device_name_);
    this->device_->serialize(buffer);
}

void io_device_container::deserialize_object(utils::buffer_deserializer& buffer)
{
    buffer.read_string(this->device_name_);
    this->setup();
    this->device_->deserialize(buffer);
}
