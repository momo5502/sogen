#pragma once

#if _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NO_POSIX_ERROR_CODES
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#else

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <cstring>

#define ZeroMemory(x, y) memset(x, 0, y)

#endif

#include <string>
#include <string_view>
#include <vector>
#include <optional>

#ifdef _WIN32
using socklen_t = int;
#pragma comment(lib, "ws2_32.lib")
#endif

namespace network
{
    void initialize_wsa();

    class address
    {
      public:
        address();
        address(std::string_view addr, const std::optional<int>& family = std::nullopt);
        address(const sockaddr_in& addr);
        address(const sockaddr_in6& addr);
        address(const sockaddr* addr, socklen_t length);

        address(const address&) = default;
        address(address&&) noexcept = default;

        address& operator=(const address&) = default;
        address& operator=(address&&) noexcept = default;

        ~address() = default;

        void set_ipv4(uint32_t ip);
        void set_ipv4(const in_addr& addr);
        void set_ipv6(const in6_addr& addr);
        void set_address(const sockaddr* addr, socklen_t length);

        void set_port(unsigned short port);
        [[nodiscard]] unsigned short get_port() const;

        sockaddr& get_addr();
        sockaddr_in& get_in_addr();
        sockaddr_in6& get_in6_addr();

        const sockaddr& get_addr() const;
        const sockaddr_in& get_in_addr() const;
        const sockaddr_in6& get_in6_addr() const;

        socklen_t get_size() const;
        socklen_t get_max_size() const;

        int get_family() const;

        bool is_ipv4() const;
        bool is_ipv6() const;
        bool is_supported() const;

        [[nodiscard]] bool is_local() const;
        [[nodiscard]] std::string to_string() const;

        bool operator==(const address& obj) const;

        bool operator!=(const address& obj) const
        {
            return !(*this == obj);
        }

        static std::vector<address> resolve_multiple(const std::string& hostname);

      private:
        union
        {
            sockaddr address_;
            sockaddr_in address4_;
            sockaddr_in6 address6_;
            sockaddr_storage storage_;
        };

        void parse(std::string_view addr, const std::optional<int>& family = {});
        void resolve(const std::string& hostname, const std::optional<int>& family = {});
    };
}

namespace std
{
    template <>
    struct hash<network::address>
    {
        std::size_t operator()(const network::address& a) const noexcept;
    };
}
