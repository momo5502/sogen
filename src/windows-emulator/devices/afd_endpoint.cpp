#include "afd_endpoint.hpp"

#include "windows-emulator/windows_emulator.hpp"

namespace
{
	struct afd_endpoint : stateless_device
	{
		NTSTATUS io_control(const io_device_context& c) override
		{
			c.win_emu.logger.print(color::cyan, "AFD IOCTL: %X\n", c.io_control_code);
			return STATUS_SUCCESS;
		}
	};
}

std::unique_ptr<io_device> create_afd_endpoint()
{
	return std::make_unique<afd_endpoint>();
}
