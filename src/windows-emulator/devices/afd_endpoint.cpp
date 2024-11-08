#include "afd_endpoint.hpp"
#include "afd_types.hpp"

#include "../windows_emulator.hpp"

#include <network/address.hpp>
#include <network/socket.hpp>

namespace
{
	struct afd_creation_data
	{
		uint64_t unk1;
		char afd_open_packet_xx[0x10];
		uint64_t unk2;
		int address_family;
		int type;
		int protocol;
		// ...
	};

	afd_creation_data get_creation_data(const io_device_creation_data& data)
	{
		if (!data.buffer || data.length < sizeof(afd_creation_data))
		{
			throw std::runtime_error("Bad AFD creation data");
		}

		return emulator_object<afd_creation_data>{data.emu, data.buffer}.read();
	}

	struct afd_endpoint : io_device
	{
		std::optional<SOCKET> s{};

		afd_endpoint()
		{
			network::initialize_wsa();
		}

		afd_endpoint(afd_endpoint&&) = delete;
		afd_endpoint& operator=(afd_endpoint&&) = delete;

		~afd_endpoint() override
		{
			if (this->s)
			{
				closesocket(*this->s);
			}
		}

		void create(const io_device_creation_data& data) override
		{
			const auto creation_data = get_creation_data(data);
			const auto sock = socket(creation_data.address_family, creation_data.type, creation_data.protocol);
			if (sock == INVALID_SOCKET)
			{
				throw std::runtime_error("Failed to create socket!");
			}

			s = sock;
		}

		void deserialize(utils::buffer_deserializer&) override
		{
			// TODO
		}

		void serialize(utils::buffer_serializer&) const override
		{
			// TODO
		}

		NTSTATUS io_control(const io_device_context& c) override
		{
			c.io_status_block.write({});

			if (_AFD_BASE(c.io_control_code) != FSCTL_AFD_BASE)
			{
				c.win_emu.logger.print(color::cyan, "Bad AFD IOCTL: %X\n", c.io_control_code);
				return STATUS_NOT_SUPPORTED;
			}

			c.win_emu.logger.print(color::cyan, "AFD IOCTL: %X\n", c.io_control_code);

			const auto request = _AFD_REQUEST(c.io_control_code);

			switch (request)
			{
			case AFD_BIND:
				return this->ioctl_bind(c);
			case AFD_SEND_DATAGRAM:
				return this->ioctl_send_datagram(c);
			case AFD_SET_CONTEXT:
				return STATUS_SUCCESS;
			case AFD_GET_INFORMATION:
				return STATUS_SUCCESS;
			}

			c.win_emu.logger.print(color::gray, "Unsupported AFD IOCTL: %X\n", c.io_control_code);
			return STATUS_NOT_SUPPORTED;
		}

		NTSTATUS ioctl_bind(const io_device_context& c) const
		{
			std::vector<std::byte> data{};
			data.resize(c.input_buffer_length);
			c.emu.read_memory(c.input_buffer, data.data(), c.input_buffer_length);

			constexpr auto address_offset = 4;

			if (data.size() < address_offset)
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const auto* address = reinterpret_cast<const sockaddr*>(data.data() + address_offset);
			const auto address_size = static_cast<int>(data.size() - address_offset);

			const network::address addr(address, address_size);

			if (bind(*this->s, &addr.get_addr(), addr.get_size()) == SOCKET_ERROR)
			{
				return STATUS_ADDRESS_ALREADY_ASSOCIATED;
			}

			return STATUS_SUCCESS;
		}

		NTSTATUS ioctl_send_datagram(const io_device_context& c) const
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

			const auto sent_data = sendto(*this->s, reinterpret_cast<const char*>(data.data()),
			                              static_cast<int>(data.size()), 0 /* ? */, &target.get_addr(),
			                              target.get_size());

			if (sent_data < 0)
			{
				return STATUS_CONNECTION_REFUSED;
			}

			if (c.io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = static_cast<uint32_t>(sent_data);
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
