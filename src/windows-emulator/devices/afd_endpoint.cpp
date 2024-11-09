#include "afd_endpoint.hpp"
#include "afd_types.hpp"

#include "../windows_emulator.hpp"

#include <network/address.hpp>
#include <network/socket.hpp>

#include <utils/finally.hpp>

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

	afd_creation_data get_creation_data(windows_emulator& win_emu, const io_device_creation_data& data)
	{
		if (!data.buffer || data.length < sizeof(afd_creation_data))
		{
			throw std::runtime_error("Bad AFD creation data");
		}

		return win_emu.emu().read_memory<afd_creation_data>(data.buffer);
	}

	struct afd_endpoint : io_device
	{
		bool in_poll{};
		std::optional<SOCKET> s{};
		std::optional<io_device_context> delayed_ioctl{};

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

		void create(windows_emulator& win_emu, const io_device_creation_data& data) override
		{
			const auto creation_data = get_creation_data(win_emu, data);
			// TODO: values map to windows values; might not be the case for other platforms
			const auto sock = socket(creation_data.address_family, creation_data.type, creation_data.protocol);
			if (sock == INVALID_SOCKET)
			{
				throw std::runtime_error("Failed to create socket!");
			}

			network::socket::set_blocking(sock, false);

			s = sock;
		}

		void work(windows_emulator& win_emu) override
		{
			if (!this->delayed_ioctl || !this->s)
			{
				return;
			}

			const auto is_ready = network::socket::is_socket_ready(*this->s, this->in_poll);
			if (!is_ready)
			{
				return;
			}

			this->execute_ioctl(win_emu, *this->delayed_ioctl);

			auto* e = win_emu.process().events.get(this->delayed_ioctl->event);
			if (e)
			{
				e->signaled = true;
			}

			this->delayed_ioctl = {};
		}

		void deserialize(utils::buffer_deserializer&) override
		{
			// TODO
		}

		void serialize(utils::buffer_serializer&) const override
		{
			// TODO
		}

		NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
		{
			if (_AFD_BASE(c.io_control_code) != FSCTL_AFD_BASE)
			{
				win_emu.logger.print(color::cyan, "Bad AFD IOCTL: %X\n", c.io_control_code);
				return STATUS_NOT_SUPPORTED;
			}

			win_emu.logger.print(color::cyan, "AFD IOCTL: %X\n", c.io_control_code);

			const auto request = _AFD_REQUEST(c.io_control_code);

			switch (request)
			{
			case AFD_BIND:
				return this->ioctl_bind(win_emu, c);
			case AFD_SEND_DATAGRAM:
				return this->ioctl_send_datagram(win_emu, c);
			case AFD_RECEIVE_DATAGRAM:
				return this->ioctl_receive_datagram(win_emu, c);
			case AFD_SET_CONTEXT:
				return STATUS_SUCCESS;
			case AFD_GET_INFORMATION:
				return STATUS_SUCCESS;
			}

			win_emu.logger.print(color::gray, "Unsupported AFD IOCTL: %X\n", c.io_control_code);
			return STATUS_NOT_SUPPORTED;
		}

		NTSTATUS ioctl_bind(windows_emulator& win_emu, const io_device_context& c) const
		{
			std::vector<std::byte> data{};
			data.resize(c.input_buffer_length);
			win_emu.emu().read_memory(c.input_buffer, data.data(), c.input_buffer_length);

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

		NTSTATUS ioctl_receive_datagram(windows_emulator& win_emu, const io_device_context& c)
		{
			auto& emu = win_emu.emu();

			if (c.input_buffer_length < sizeof(AFD_RECV_DATAGRAM_INFO))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const auto receive_info = emu.read_memory<AFD_RECV_DATAGRAM_INFO>(c.input_buffer);
			const auto buffer = emu.read_memory<WSABUF>(receive_info.BufferArray);

			std::vector<std::byte> address{};

			ULONG address_length = 0x1000;
			if (receive_info.AddressLength)
			{
				address_length = emu.read_memory<ULONG>(receive_info.AddressLength);
			}

			address.resize(std::clamp(address_length, 1UL, 0x1000UL));

			if (!buffer.len || buffer.len > 0x10000 || !buffer.buf)
			{
				return STATUS_INVALID_PARAMETER;
			}

			int fromlength = static_cast<int>(address.size());

			std::vector<char> data{};
			data.resize(buffer.len);

			const auto recevied_data = recvfrom(*this->s, data.data(), static_cast<int>(data.size()), 0,
			                                    reinterpret_cast<sockaddr*>(address.data()), &fromlength);

			if (recevied_data < 0)
			{
				const auto error = GET_SOCKET_ERROR();
				if (error == SOCK_WOULDBLOCK)
				{
					this->in_poll = true;
					this->delayed_ioctl = c;
					return STATUS_PENDING;
				}

				return STATUS_UNSUCCESSFUL;
			}

			emu.write_memory(reinterpret_cast<uint64_t>(buffer.buf), data.data(),
			                 std::min(data.size(), static_cast<size_t>(recevied_data)));

			if (receive_info.Address && address_length)
			{
				emu.write_memory(reinterpret_cast<uint64_t>(receive_info.Address), address.data(),
				                 std::min(address.size(), static_cast<size_t>(address_length)));
			}

			if (c.io_status_block)
			{
				IO_STATUS_BLOCK block{};
				block.Information = static_cast<uint32_t>(recevied_data);
				c.io_status_block.write(block);
			}

			return STATUS_SUCCESS;
		}

		NTSTATUS ioctl_send_datagram(windows_emulator& win_emu, const io_device_context& c)
		{
			auto& emu = win_emu.emu();

			if (c.input_buffer_length < sizeof(AFD_SEND_DATAGRAM_INFO))
			{
				return STATUS_BUFFER_TOO_SMALL;
			}

			const auto send_info = emu.read_memory<AFD_SEND_DATAGRAM_INFO>(c.input_buffer);
			const auto buffer = emu.read_memory<WSABUF>(send_info.BufferArray);

			std::vector<std::byte> address{};
			address.resize(send_info.TdiConnInfo.RemoteAddressLength);
			emu.read_memory(reinterpret_cast<uint64_t>(send_info.TdiConnInfo.RemoteAddress), address.data(),
			                address.size());

			const network::address target(reinterpret_cast<sockaddr*>(address.data()),
			                              static_cast<int>(address.size()));

			std::vector<std::byte> data{};
			data.resize(buffer.len);
			emu.read_memory(reinterpret_cast<uint64_t>(buffer.buf), data.data(), data.size());

			const auto sent_data = sendto(*this->s, reinterpret_cast<const char*>(data.data()),
			                              static_cast<int>(data.size()), 0 /* ? */, &target.get_addr(),
			                              target.get_size());

			if (sent_data < 0)
			{
				const auto error = GET_SOCKET_ERROR();
				if (error == SOCK_WOULDBLOCK)
				{
					this->in_poll = false;
					this->delayed_ioctl = c;
					return STATUS_PENDING;
				}

				return STATUS_UNSUCCESSFUL;
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
