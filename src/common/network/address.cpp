#include "address.hpp"

#include <stdexcept>

#include "../utils/finally.hpp"

using namespace std::literals;

namespace network
{
    void initialize_wsa()
    {
#ifdef _WIN32
        static struct wsa_initializer
        {
            wsa_initializer()
            {
                WSADATA wsa_data;
                if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
                {
                    throw std::runtime_error("Unable to initialize WSA");
                }
            }

            ~wsa_initializer()
            {
                WSACleanup();
            }
        } _;
#endif
    }

    address::address()
    {
        initialize_wsa();
        ZeroMemory(&this->storage_, this->get_max_size());

        this->address_.sa_family = AF_UNSPEC;
    }

    address::address(const std::string_view addr, const std::optional<int>& family)
        : address()
    {
        this->parse(addr, family);
    }

    address::address(const sockaddr_in6& addr)
        : address()
    {
        this->address6_ = addr;
    }

    address::address(const sockaddr_in& addr)
        : address()
    {
        this->address4_ = addr;
    }

    address::address(const sockaddr* addr, const socklen_t length)
        : address()
    {
        this->set_address(addr, length);
    }

    void address::set_ipv4(const uint32_t ip)
    {
        in_addr addr{};
        addr.s_addr = ip;
        this->set_ipv4(addr);
    }

    bool address::operator==(const address& obj) const
    {
        if (this->get_family() != obj.get_family())
        {
            return false;
        }

        if (this->get_port() != obj.get_port())
        {
            return false;
        }

        if (this->is_ipv4())
        {
            return this->address4_.sin_addr.s_addr == obj.address4_.sin_addr.s_addr;
        }

        if (this->is_ipv6())
        {
            return !memcmp(this->address6_.sin6_addr.s6_addr, obj.address6_.sin6_addr.s6_addr,
                           sizeof(obj.address6_.sin6_addr.s6_addr));
        }

        return false;
    }

    void address::set_ipv4(const in_addr& addr)
    {
        ZeroMemory(&this->address4_, sizeof(this->address4_));
        this->address4_.sin_family = AF_INET;
        this->address4_.sin_addr = addr;
    }

    void address::set_ipv6(const in6_addr& addr)
    {
        ZeroMemory(&this->address6_, sizeof(this->address6_));
        this->address6_.sin6_family = AF_INET6;
        this->address6_.sin6_addr = addr;
    }

    void address::set_address(const sockaddr* addr, const socklen_t length)
    {
        if (static_cast<size_t>(length) >= sizeof(sockaddr_in) && addr->sa_family == AF_INET)
        {
            this->address4_ = *reinterpret_cast<const sockaddr_in*>(addr);
        }
        else if (static_cast<size_t>(length) == sizeof(sockaddr_in6) && addr->sa_family == AF_INET6)
        {
            this->address6_ = *reinterpret_cast<const sockaddr_in6*>(addr);
        }
        else
        {
            throw std::runtime_error("Invalid network address");
        }
    }

    void address::set_port(const unsigned short port)
    {
        switch (this->get_family())
        {
        case AF_INET:
            this->address4_.sin_port = htons(port);
            break;
        case AF_INET6:
            this->address6_.sin6_port = htons(port);
            break;
        default:
            throw std::runtime_error("Invalid address family");
        }
    }

    unsigned short address::get_port() const
    {
        switch (this->get_family())
        {
        case AF_INET:
            return ntohs(this->address4_.sin_port);
        case AF_INET6:
            return ntohs(this->address6_.sin6_port);
        default:
            return 0;
        }
    }

    std::string address::to_string() const
    {
        char buffer[1000] = {};
        std::string addr;

        switch (this->get_family())
        {
        case AF_INET:
            inet_ntop(this->get_family(), &this->address4_.sin_addr, buffer, sizeof(buffer));
            addr = std::string(buffer);
            break;
        case AF_INET6:
            inet_ntop(this->get_family(), &this->address6_.sin6_addr, buffer, sizeof(buffer));
            addr = "[" + std::string(buffer) + "]";
            break;
        default:
            buffer[0] = '?';
            buffer[1] = 0;
            addr = std::string(buffer);
            break;
        }

        return addr + ":"s + std::to_string(this->get_port());
    }

    bool address::is_local() const
    {
        if (!this->is_ipv4())
        {
            return false;
        }

        // According to: https://en.wikipedia.org/wiki/Private_network

        uint8_t bytes[4];
        memcpy(bytes, &this->address4_.sin_addr.s_addr, sizeof(bytes));

        // 10.X.X.X
        if (bytes[0] == 10)
        {
            return true;
        }

        // 192.168.X.X
        if (bytes[0] == 192 && bytes[1] == 168)
        {
            return true;
        }

        // 172.16.X.X - 172.31.X.X
        if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] < 32)
        {
            return true;
        }

        // 127.0.0.1
        if (this->address4_.sin_addr.s_addr == 0x0100007F)
        {
            return true;
        }

        return false;
    }

    sockaddr& address::get_addr()
    {
        return this->address_;
    }

    const sockaddr& address::get_addr() const
    {
        return this->address_;
    }

    sockaddr_in& address::get_in_addr()
    {
        return this->address4_;
    }

    sockaddr_in6& address::get_in6_addr()
    {
        return this->address6_;
    }

    const sockaddr_in& address::get_in_addr() const
    {
        return this->address4_;
    }

    const sockaddr_in6& address::get_in6_addr() const
    {
        return this->address6_;
    }

    socklen_t address::get_size() const
    {
        switch (this->get_family())
        {
        case AF_INET:
            return static_cast<socklen_t>(sizeof(this->address4_));
        case AF_INET6:
            return static_cast<socklen_t>(sizeof(this->address6_));
        default:
            return static_cast<socklen_t>(sizeof(this->address_));
        }
    }

    socklen_t address::get_max_size() const
    {
        constexpr auto s = sizeof(this->address_);
        constexpr auto s4 = sizeof(this->address4_);
        constexpr auto s6 = sizeof(this->address6_);
        constexpr auto sstore = sizeof(this->storage_);
        constexpr auto max_size = std::max(sstore, std::max(s, std::max(s4, s6)));
        static_assert(max_size == sstore);

        return static_cast<socklen_t>(max_size);
    }

    int address::get_family() const
    {
        return this->address_.sa_family;
    }

    bool address::is_ipv4() const
    {
        return this->get_family() == AF_INET;
    }

    bool address::is_ipv6() const
    {
        return this->get_family() == AF_INET6;
    }

    bool address::is_supported() const
    {
        return is_ipv4() || is_ipv6();
    }

    void address::parse(std::string_view addr, const std::optional<int>& family)
    {
        std::optional<uint16_t> port_value{};

        const auto pos = addr.find_last_of(':');
        if (pos != std::string::npos)
        {
            const auto port = addr.substr(pos + 1);
            port_value = static_cast<uint16_t>(atoi(port.data()));
            addr = addr.substr(0, pos);
        }

        this->resolve(std::string(addr), family);

        if (port_value)
        {
            this->set_port(*port_value);
        }
    }

    void address::resolve(const std::string& hostname, const std::optional<int>& family)
    {
        const auto port = this->get_port();
        auto port_reset_action = utils::finally([this, port]() { this->set_port(port); });

        const auto result = resolve_multiple(hostname);
        for (const auto& addr : result)
        {
            if (addr.is_supported() && (!family || addr.get_addr().sa_family == *family))
            {
                this->set_address(&addr.get_addr(), addr.get_size());
                return;
            }
        }

        port_reset_action.cancel();
        throw std::runtime_error{"Unable to resolve hostname: " + hostname};
    }

    std::vector<address> address::resolve_multiple(const std::string& hostname)
    {
        std::vector<address> results{};

        addrinfo* result = nullptr;
        if (!getaddrinfo(hostname.data(), nullptr, nullptr, &result))
        {
            const auto _2 = utils::finally([&result] { freeaddrinfo(result); });

            for (const auto* i = result; i; i = i->ai_next)
            {
                if (i->ai_family == AF_INET || i->ai_family == AF_INET6)
                {
                    address a{};
                    a.set_address(i->ai_addr, static_cast<socklen_t>(i->ai_addrlen));
                    results.emplace_back(std::move(a));
                }
            }
        }

        return results;
    }
}

std::size_t std::hash<network::address>::operator()(const network::address& a) const noexcept
{
    const uint32_t family = a.get_addr().sa_family;
    const uint32_t port = a.get_port();

    std::size_t hash = std::hash<uint32_t>{}(family);
    hash ^= std::hash<uint32_t>{}(port);
    switch (a.get_addr().sa_family)
    {
    case AF_INET:
        hash ^= std::hash<decltype(a.get_in_addr().sin_addr.s_addr)>{}(a.get_in_addr().sin_addr.s_addr);
        break;
    case AF_INET6:
        hash ^= std::hash<std::string_view>{}(
            std::string_view{reinterpret_cast<const char*>(a.get_in6_addr().sin6_addr.s6_addr),
                             sizeof(a.get_in6_addr().sin6_addr.s6_addr)});
        break;
    }

    return hash;
}
