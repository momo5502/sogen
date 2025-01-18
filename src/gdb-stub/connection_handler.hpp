#pragma once
#include "stream_processor.hpp"

#include <utils/concurrency.hpp>
#include <network/tcp_client_socket.hpp>

#include <thread>
#include <functional>
#include <condition_variable>

namespace gdb_stub
{
    class connection_handler
    {
      public:
        connection_handler(network::tcp_client_socket& client);
        ~connection_handler();

        connection_handler(connection_handler&&) = delete;
        connection_handler(const connection_handler&) = delete;

        connection_handler& operator=(connection_handler&&) = delete;
        connection_handler& operator=(const connection_handler&) = delete;

        std::optional<std::string> get_packet();

        void send_reply(std::string_view data);
        void send_raw_data(std::string_view data);

        void close() const;

        bool should_stop() const;

      private:
        network::tcp_client_socket& client_;
        stream_processor processor_{};

        std::mutex mutex_{};
        std::atomic_bool stop_{};
        std::string output_stream_{};
        std::thread output_thread_{};
        std::condition_variable condition_variable_{};

        void transmission_loop();
        void await_transmission(const std::function<void()>& handler);
        std::string get_next_data_to_transmit();
    };
}
