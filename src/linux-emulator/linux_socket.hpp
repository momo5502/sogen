#pragma once

#include "std_include.hpp"

#include <network/socket.hpp>

namespace sogen
{

    struct linux_socket_state
    {
        int domain{};
        int type{};
        int protocol{};
        bool listening{};
        bool connected{};
        int so_reuseaddr{};
        int so_keepalive{};
        int so_reuseport{};
        int so_sndbuf{65536};
        int so_rcvbuf{65536};
        int so_error{};

        uint16_t bound_port{};
        uint32_t bound_addr{};

        uint16_t peer_port{};
        uint32_t peer_addr{};

        std::unique_ptr<network::socket> host_socket{};
    };

    class linux_socket_table
    {
      public:
        linux_socket_state& emplace(const int fd, linux_socket_state&& state)
        {
            return this->states_.emplace(fd, std::move(state)).first->second;
        }

        linux_socket_state* get(const int fd)
        {
            const auto it = this->states_.find(fd);
            if (it == this->states_.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        const linux_socket_state* get(const int fd) const
        {
            const auto it = this->states_.find(fd);
            if (it == this->states_.end())
            {
                return nullptr;
            }

            return &it->second;
        }

        void erase(const int fd)
        {
            auto it = this->states_.find(fd);
            if (it == this->states_.end())
            {
                return;
            }

            if (it->second.host_socket)
            {
                it->second.host_socket->close();
            }

            this->states_.erase(it);
        }

      private:
        std::map<int, linux_socket_state> states_{};
    };

} // namespace sogen
