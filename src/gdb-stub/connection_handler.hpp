#pragma once
#include "stream_processor.hpp"
#include <network/tcp_client_socket.hpp>

namespace gdb_stub
{
    class connection_handler
    {
      public:
        connection_handler(network::tcp_client_socket& client);

        std::optional<std::string> get_packet();

        void send_packet(std::string_view data) const;
        void send_raw_data(std::string_view data) const;

        void close() const;

      private:
        network::tcp_client_socket& client_;
        stream_processor processor_{};
    };
}
