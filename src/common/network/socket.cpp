#include "socket.hpp"

#include <thread>

#ifdef _WIN32
#define poll WSAPoll
#define SOCK_WOULDBLOCK WSAEWOULDBLOCK
#else
#define SOCK_WOULDBLOCK EWOULDBLOCK
#endif

using namespace std::literals;

namespace network
{
	socket::socket(const int af)
		: address_family_(af)
	{
		initialize_wsa();
		this->socket_ = ::socket(af, SOCK_DGRAM, IPPROTO_UDP);

		if (af == AF_INET6)
		{
			int i = 1;
			setsockopt(this->socket_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&i),
			           static_cast<int>(sizeof(i)));
		}
	}

	socket::~socket()
	{
		if (this->socket_ != INVALID_SOCKET)
		{
#ifdef _WIN32
			closesocket(this->socket_);
#else
			close(this->socket_);
#endif
		}
	}

	socket::socket(socket&& obj) noexcept
	{
		this->operator=(std::move(obj));
	}

	socket& socket::operator=(socket&& obj) noexcept
	{
		if (this != &obj)
		{
			this->~socket();
			this->socket_ = obj.socket_;
			this->port_ = obj.port_;
			this->address_family_ = obj.address_family_;

			obj.socket_ = INVALID_SOCKET;
			obj.address_family_ = AF_UNSPEC;
		}

		return *this;
	}

	bool socket::bind_port(const address& target)
	{
		const auto result = bind(this->socket_, &target.get_addr(), target.get_size()) == 0;
		if (result)
		{
			this->port_ = target.get_port();
		}

		return result;
	}

	bool socket::send(const address& target, const void* data, const size_t size) const
	{
		const int res = sendto(this->socket_, static_cast<const char*>(data), static_cast<int>(size), 0,
		                       &target.get_addr(),
		                       target.get_size());
		return res == static_cast<int>(size);
	}

	bool socket::send(const address& target, const std::string& data) const
	{
		return this->send(target, data.data(), data.size());
	}

	bool socket::receive(address& source, std::string& data) const
	{
		char buffer[0x2000];
		socklen_t len = source.get_max_size();

		const auto result = recvfrom(this->socket_, buffer, static_cast<int>(sizeof(buffer)), 0, &source.get_addr(),
		                             &len);
		if (result == SOCKET_ERROR)
		{
			return false;
		}

		data.assign(buffer, buffer + result);
		return true;
	}

	bool socket::set_blocking(const bool blocking)
	{
#ifdef _WIN32
		unsigned long mode = blocking ? 0 : 1;
		return ioctlsocket(this->socket_, FIONBIO, &mode) == 0;
#else
		int flags = fcntl(this->socket_, F_GETFL, 0);
		if (flags == -1) return false;
		flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
		return fcntl(this->socket_, F_SETFL, flags) == 0;
#endif
	}

	bool socket::sleep(const std::chrono::milliseconds timeout) const
	{
		/*fd_set fdr;
		FD_ZERO(&fdr);
		FD_SET(this->socket_, &fdr);

		const auto msec = timeout.count();

		timeval tv{};
		tv.tv_sec = static_cast<long>(msec / 1000ll);
		tv.tv_usec = static_cast<long>((msec % 1000) * 1000);

		const auto retval = select(static_cast<int>(this->socket_) + 1, &fdr, nullptr, nullptr, &tv);
		if (retval == SOCKET_ERROR)
		{
			std::this_thread::sleep_for(1ms);
			return socket_is_ready;
		}

		if (retval > 0)
		{
			return socket_is_ready;
		}

		return !socket_is_ready;*/

		std::vector<const socket*> sockets{};
		sockets.push_back(this);

		return sleep_sockets(sockets, timeout);
	}

	bool socket::sleep_until(const std::chrono::high_resolution_clock::time_point time_point) const
	{
		const auto duration = time_point - std::chrono::high_resolution_clock::now();
		return this->sleep(std::chrono::duration_cast<std::chrono::milliseconds>(duration));
	}

	SOCKET socket::get_socket() const
	{
		return this->socket_;
	}

	uint16_t socket::get_port() const
	{
		return this->port_;
	}

	int socket::get_address_family() const
	{
		return this->address_family_;
	}

	bool socket::sleep_sockets(const std::span<const socket*>& sockets, const std::chrono::milliseconds timeout)
	{
		std::vector<pollfd> pfds{};
		pfds.resize(sockets.size());

		for (size_t i = 0; i < sockets.size(); ++i)
		{
			auto& pfd = pfds.at(i);
			const auto& socket = sockets[i];

			pfd.fd = socket->get_socket();
			pfd.events = POLLIN;
			pfd.revents = 0;
		}

		const auto retval = poll(pfds.data(), static_cast<uint32_t>(pfds.size()),
		                              static_cast<int>(timeout.count()));

		if (retval == SOCKET_ERROR)
		{
			std::this_thread::sleep_for(1ms);
			return socket_is_ready;
		}

		if (retval > 0)
		{
			return socket_is_ready;
		}

		return !socket_is_ready;
	}

	bool socket::sleep_sockets_until(const std::span<const socket*>& sockets,
	                                 const std::chrono::high_resolution_clock::time_point time_point)
	{
		const auto duration = time_point - std::chrono::high_resolution_clock::now();
		return sleep_sockets(sockets, std::chrono::duration_cast<std::chrono::milliseconds>(duration));
	}
}
