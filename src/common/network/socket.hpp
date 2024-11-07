#pragma once

#include "address.hpp"

#include <span>
#include <chrono>

#ifdef _WIN32
using socklen_t = int;
#define GET_SOCKET_ERROR() (WSAGetLastError())
#else
		using SOCKET = int;
#define INVALID_SOCKET  (SOCKET)(~0)
#define SOCKET_ERROR            (-1)
#define GET_SOCKET_ERROR() (errno)
#endif

namespace network
{
	class socket
	{
	public:
		socket() = default;

		socket(int af);
		~socket();

		socket(const socket& obj) = delete;
		socket& operator=(const socket& obj) = delete;

		socket(socket&& obj) noexcept;
		socket& operator=(socket&& obj) noexcept;

		bool bind_port(const address& target);

		[[maybe_unused]] bool send(const address& target, const void* data, size_t size) const;
		[[maybe_unused]] bool send(const address& target, const std::string& data) const;
		bool receive(address& source, std::string& data) const;

		bool set_blocking(bool blocking);

		static constexpr bool socket_is_ready = true;
		bool sleep(std::chrono::milliseconds timeout) const;
		bool sleep_until(std::chrono::high_resolution_clock::time_point time_point) const;

		SOCKET get_socket() const;
		uint16_t get_port() const;

		int get_address_family() const;

		static bool sleep_sockets(const std::span<const socket*>& sockets, std::chrono::milliseconds timeout);
		static bool sleep_sockets_until(const std::span<const socket*>& sockets,
		                                std::chrono::high_resolution_clock::time_point time_point);

	private:
		int address_family_{AF_UNSPEC};
		uint16_t port_ = 0;
		SOCKET socket_ = INVALID_SOCKET;
	};
}
