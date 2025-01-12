#include "socket.hpp"
#include "address.hpp"

#include <thread>

using namespace std::literals;

namespace network
{
    socket::socket(const SOCKET s)
        : socket_(s)
    {
    }

    socket::socket(const int af, const int type, const int protocol)
    {
        initialize_wsa();
        this->socket_ = ::socket(af, type, protocol);

        if (af == AF_INET6)
        {
            int i = 1;
            setsockopt(this->socket_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&i),
                       static_cast<int>(sizeof(i)));
        }

        int optval = 1;
        setsockopt(this->socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&optval),
                   static_cast<int>(sizeof(optval)));
    }

    socket::~socket()
    {
        this->close();
    }

    socket::socket(socket&& obj) noexcept
    {
        this->operator=(std::move(obj));
    }

    socket& socket::operator=(socket&& obj) noexcept
    {
        if (this != &obj)
        {
            this->close();
            this->socket_ = obj.socket_;

            obj.socket_ = INVALID_SOCKET;
        }

        return *this;
    }

    socket::operator bool() const
    {
        return this->is_valid();
    }

    bool socket::is_valid() const
    {
        return this->socket_ != INVALID_SOCKET;
    }

    void socket::close()
    {
        if (this->socket_ != INVALID_SOCKET)
        {
            ::closesocket(this->socket_);
            this->socket_ = INVALID_SOCKET;
        }
    }

    bool socket::bind(const address& target)
    {
        return ::bind(this->socket_, &target.get_addr(), target.get_size()) == 0;
    }

    bool socket::set_blocking(const bool blocking)
    {
        return socket::set_blocking(this->socket_, blocking);
    }

    bool socket::set_blocking(SOCKET s, const bool blocking)
    {
#ifdef _WIN32
        unsigned long mode = blocking ? 0 : 1;
        return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
        int flags = fcntl(s, F_GETFL, 0);
        if (flags == -1)
            return false;
        flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
        return fcntl(s, F_SETFL, flags) == 0;
#endif
    }

    bool socket::sleep(const std::chrono::milliseconds timeout, const bool in_poll) const
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

        return sleep_sockets(sockets, timeout, in_poll);
    }

    bool socket::sleep_until(const std::chrono::high_resolution_clock::time_point time_point, const bool in_poll) const
    {
        const auto duration = time_point - std::chrono::high_resolution_clock::now();
        return this->sleep(std::chrono::duration_cast<std::chrono::milliseconds>(duration), in_poll);
    }

    SOCKET socket::get_socket() const
    {
        return this->socket_;
    }

    std::optional<address> socket::get_name() const
    {
        address a{};
        auto len = a.get_max_size();
        if (getsockname(this->socket_, &a.get_addr(), &len) == SOCKET_ERROR)
        {
            return std::nullopt;
        }

        return a;
    }

    uint16_t socket::get_port() const
    {
        const auto address = this->get_name();
        if (!address)
        {
            return 0;
        }

        return address->get_port();
    }

    int socket::get_address_family() const
    {
        const auto address = this->get_name();
        if (!address)
        {
            return AF_UNSPEC;
        }

        return address->get_addr().sa_family;
    }

    bool socket::is_ready(const bool in_poll) const
    {
        return this->is_valid() && is_socket_ready(this->socket_, in_poll);
    }

    bool socket::sleep_sockets(const std::span<const socket*>& sockets, const std::chrono::milliseconds timeout,
                               const bool in_poll)
    {
        std::vector<pollfd> pfds{};
        pfds.resize(sockets.size());

        for (size_t i = 0; i < sockets.size(); ++i)
        {
            auto& pfd = pfds.at(i);
            const auto& socket = sockets[i];

            pfd.fd = socket->get_socket();
            pfd.events = in_poll ? POLLIN : POLLOUT;
            pfd.revents = 0;
        }

        const auto retval = poll(pfds.data(), static_cast<uint32_t>(pfds.size()), static_cast<int>(timeout.count()));

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

    bool socket::is_socket_ready(const SOCKET s, const bool in_poll)
    {
        pollfd pfd{};

        pfd.fd = s;
        pfd.events = in_poll ? POLLIN : POLLOUT;
        pfd.revents = 0;

        const auto retval = poll(&pfd, 1, 0);

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
                                     const std::chrono::high_resolution_clock::time_point time_point,
                                     const bool in_poll)
    {
        const auto duration = time_point - std::chrono::high_resolution_clock::now();
        return sleep_sockets(sockets, std::chrono::duration_cast<std::chrono::milliseconds>(duration), in_poll);
    }
}
