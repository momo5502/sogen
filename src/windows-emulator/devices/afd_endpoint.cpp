#include "afd_endpoint.hpp"

#include "../windows_emulator.hpp"
#include <network/address.hpp>
#include <network/socket.hpp>


typedef LONG TDI_STATUS;
typedef PVOID CONNECTION_CONTEXT;

typedef struct _TDI_CONNECTION_INFORMATION
{
	LONG UserDataLength;
	PVOID UserData;
	LONG OptionsLength;
	PVOID Options;
	LONG RemoteAddressLength;
	PVOID RemoteAddress;
} TDI_CONNECTION_INFORMATION, *PTDI_CONNECTION_INFORMATION;

typedef struct _TDI_REQUEST
{
	union
	{
		HANDLE AddressHandle;
		CONNECTION_CONTEXT ConnectionContext;
		HANDLE ControlChannel;
	} Handle;

	PVOID RequestNotifyObject;
	PVOID RequestContext;
	TDI_STATUS TdiStatus;
} TDI_REQUEST, *PTDI_REQUEST;

typedef struct _TDI_REQUEST_SEND_DATAGRAM
{
	TDI_REQUEST Request;
	PTDI_CONNECTION_INFORMATION SendDatagramInformation;
} TDI_REQUEST_SEND_DATAGRAM, *PTDI_REQUEST_SEND_DATAGRAM;

typedef struct _AFD_SEND_DATAGRAM_INFO
{
	LPWSABUF BufferArray;
	ULONG BufferCount;
	ULONG AfdFlags;
	TDI_REQUEST_SEND_DATAGRAM TdiRequest;
	TDI_CONNECTION_INFORMATION TdiConnInfo;
} AFD_SEND_DATAGRAM_INFO, *PAFD_SEND_DATAGRAM_INFO;

namespace
{
	struct afd_endpoint : stateless_device
	{
		network::socket s{AF_INET};

		NTSTATUS io_control(const io_device_context& c) override
		{
			c.win_emu.logger.print(color::cyan, "AFD IOCTL: %X\n", c.io_control_code);

			switch (c.io_control_code)
			{
			case 0x12003:
				return this->ioctl_bind(c);
			case 0x12023:
				return this->ioctl_send_datagram(c);
			case 0x12047: // ?
			case 0x1207B: // ?
				return STATUS_SUCCESS;
			}

			return STATUS_SUCCESS;
		}

		NTSTATUS ioctl_bind(const io_device_context& c)
		{
			std::vector<std::byte> data{};
			data.resize(c.input_buffer_length);
			c.emu.read_memory(c.input_buffer, data.data(), c.input_buffer_length);

			utils::buffer_deserializer deserializer{data, true};
			deserializer.read<uint32_t>(); // IDK :(
			const network::address addr = deserializer.read<sockaddr_in>();

			if (!this->s.bind_port(addr))
			{
				return STATUS_ADDRESS_ALREADY_ASSOCIATED;
			}

			return STATUS_SUCCESS;
		}

		NTSTATUS ioctl_send_datagram(const io_device_context& c)
		{
			if (c.input_buffer_length < sizeof(AFD_SEND_DATAGRAM_INFO))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const auto send_info = emulator_object<AFD_SEND_DATAGRAM_INFO>{c.emu, c.input_buffer}.read();
			const auto buffer = emulator_object<WSABUF>{c.emu, send_info.BufferArray}.read(0);

			std::vector<std::byte> address{};
			address.resize(send_info.TdiConnInfo.RemoteAddressLength);
			c.emu.read_memory(reinterpret_cast<uint64_t>(send_info.TdiConnInfo.RemoteAddress), address.data(),
			                  address.size());

			const network::address target(reinterpret_cast<sockaddr*>(address.data()),
			                              static_cast<int>(address.size()));

			std::vector<std::byte> data{};
			data.resize(buffer.len);
			c.emu.read_memory(reinterpret_cast<uint64_t>(buffer.buf), data.data(), data.size());

			if (!s.send(target, data.data(), data.size()))
			{
				return STATUS_CONNECTION_REFUSED;
			}

			if (c.io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = data.size();
				c.io_status_block.write(block);
			}

			return STATUS_SUCCESS;
		}
	};
}

std::unique_ptr<io_device> create_afd_endpoint()
{
	return std::make_unique<afd_endpoint>();
}
