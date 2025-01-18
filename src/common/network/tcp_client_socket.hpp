#pragma once

#include "socket.hpp"

#include <string_view>

namespace network
{
    class tcp_server_socket;

    class tcp_client_socket : public socket
    {
      public:
        tcp_client_socket(int af);

        tcp_client_socket() = default;
        ~tcp_client_socket() override;

        tcp_client_socket(tcp_client_socket&& obj) noexcept = default;
        tcp_client_socket& operator=(tcp_client_socket&& obj) noexcept = default;

        [[maybe_unused]] bool send(const void* data, size_t size) const;
        [[maybe_unused]] bool send(std::string_view data) const;
        std::optional<std::string> receive(std::optional<size_t> max_size = std::nullopt);

        std::optional<address> get_target() const;

        bool connect(const address& target);

      private:
        friend tcp_server_socket;
        tcp_client_socket(SOCKET s, const address& target);
    };
}
