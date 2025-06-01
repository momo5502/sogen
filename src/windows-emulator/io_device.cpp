#include "std_include.hpp"
#include "io_device.hpp"
#include "devices/afd_endpoint.hpp"
#include "devices/mount_point_manager.hpp"
#include "devices/security_support_provider.hpp"

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
    if (device == u"CNG"                 //
        || device == u"Nsi"              //
        || device == u"RasAcd"           //
        || device == u"PcwDrv"           //
        || device == u"DeviceApi\\CMApi" //
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

    throw std::runtime_error("Unsupported device: " + u16_to_u8(device));
}
