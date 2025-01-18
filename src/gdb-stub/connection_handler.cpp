#include "connection_handler.hpp"
#include "checksum.hpp"
#include <utils/string.hpp>

#include <thread>

using namespace std::literals;

namespace gdb_stub
{
    namespace
    {
        bool read_from_socket(stream_processor& processor, network::tcp_client_socket& client)
        {
            const auto data = client.receive();
            if (!data)
            {
                return false;
            }

            processor.push_stream_data(*data);
            return true;
        }
    }

    connection_handler::connection_handler(network::tcp_client_socket& client)
        : client_(client)
    {
        this->client_.set_blocking(false);
    }

    std::optional<std::string> connection_handler::get_packet()
    {
        while (this->client_.is_valid() && !this->processor_.has_packet())
        {
            if (!read_from_socket(this->processor_, this->client_))
            {
                std::this_thread::sleep_for(100ms);
            }
        }

        if (this->processor_.has_packet())
        {
            return this->processor_.get_next_packet();
        }

        return std::nullopt;
    }

    void connection_handler::send_reply(const std::string_view data) const
    {
        const auto checksum = utils::string::to_hex_string(compute_checksum(data));
        this->send_raw_data("$" + std::string(data) + "#" + checksum);
    }

    void connection_handler::send_raw_data(const std::string_view data) const
    {
        (void)this->client_.send(data);
    }

    void connection_handler::close() const
    {
        this->client_.close();
    }
}
