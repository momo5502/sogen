#pragma once

#include "address.hpp"

#include <span>
#include <chrono>
#include <optional>

#ifdef _WIN32
using send_size = int;
#define GET_SOCKET_ERROR() (WSAGetLastError())
#define poll               WSAPoll
#define SERR(x)            (WSA##x)
#define SHUT_RDWR          SD_BOTH
#else
using SOCKET = int;
using send_size = size_t;
#define INVALID_SOCKET     (SOCKET)(~0)
#define SOCKET_ERROR       (-1)
#define GET_SOCKET_ERROR() (errno)
#define closesocket        close
#define SERR(x)            (x)
#endif

namespace network
{
    class socket
    {
      public:
        socket() = default;

        socket(SOCKET s);

        socket(int af, int type, int protocol);
        virtual ~socket();

        socket(const socket& obj) = delete;
        socket& operator=(const socket& obj) = delete;

        socket(socket&& obj) noexcept;
        socket& operator=(socket&& obj) noexcept;

        operator bool() const;

        bool is_valid() const;

        bool bind(const address& target);

        bool set_blocking(bool blocking);
        static bool set_blocking(SOCKET s, bool blocking);

        static constexpr bool socket_is_ready = true;
        bool sleep(std::chrono::milliseconds timeout, bool in_poll = true) const;
        bool sleep_until(std::chrono::high_resolution_clock::time_point time_point, bool in_poll = true) const;

        SOCKET get_socket() const;
        uint16_t get_port() const;
        std::optional<address> get_name() const;

        int get_address_family() const;

        bool is_ready(bool in_poll) const;

        static bool sleep_sockets(const std::span<const socket*>& sockets, std::chrono::milliseconds timeout,
                                  bool in_poll);
        static bool sleep_sockets_until(const std::span<const socket*>& sockets,
                                        std::chrono::high_resolution_clock::time_point time_point, bool in_poll);

        static bool is_socket_ready(SOCKET s, bool in_poll);

        void close();

      private:
        SOCKET socket_ = INVALID_SOCKET;
    };
}
