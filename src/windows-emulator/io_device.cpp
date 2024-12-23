#include "io_device.hpp"
#include "devices/afd_endpoint.hpp"

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

std::unique_ptr<io_device> create_device(const std::wstring_view device)
{
	if (device == L"CNG"
		|| device == L"KsecDD"
		|| device == L"PcwDrv"
		|| device == L"DeviceApi\\CMApi"
		|| device == L"ConDrv\\Server")
	{
		return std::make_unique<dummy_device>();
	}

	if (device == L"Afd\\Endpoint")
	{
		return create_afd_endpoint();
	}

	throw std::runtime_error("Unsupported device: " + std::string(device.begin(), device.end()));
}
