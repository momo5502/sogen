#include "gdb_stub.hpp"

#include <cassert>
#include <queue>
#include <network/tcp_server_socket.hpp>

namespace gdb_stub
{
    namespace
    {
        constexpr size_t CHECKSUM_SIZE = 2;

        uint8_t compute_checksum(const std::string_view data)
        {
            uint8_t csum = 0;
            for (const auto c : data)
            {
                csum += static_cast<uint8_t>(c);
            }

            return csum;
        }

        network::tcp_client_socket accept_client(const network::address& bind_address)
        {
            network::tcp_server_socket server{bind_address.get_family()};
            if (!server.bind(bind_address))
            {
                return false;
            }

            return server.accept();
        }

        struct packet_queue
        {
            std::string buffer{};
            std::queue<std::string> packets{};

            void enqueue(const std::string& data)
            {
                buffer.append(data);
                this->process();
            }

            void process()
            {
                while (true)
                {
                    this->trim_start();

                    const auto end = this->buffer.find_first_of('#');
                    if (end == std::string::npos)
                    {
                        break;
                    }

                    const auto packet_size = end + CHECKSUM_SIZE + 1;

                    if (packet_size > this->buffer.size())
                    {
                        break;
                    }

                    auto packet = this->buffer.substr(0, packet_size);
                    this->buffer.erase(0, packet_size);

                    this->enqueue_packet(std::move(packet));
                }
            }

            void enqueue_packet(std::string packet)
            {
                constexpr auto END_BYTES = CHECKSUM_SIZE + 1;

                if (packet.size() < (END_BYTES + 1) //
                    || packet.front() != '$'        //
                    || packet[packet.size() - END_BYTES] != '#')
                {
                    return;
                }

                const auto checksum = strtoul(packet.c_str() + packet.size() - CHECKSUM_SIZE, nullptr, 16);
                assert((checksum & 0xFF) == checksum);

                packet.erase(packet.begin());
                packet.erase(packet.size() - END_BYTES, END_BYTES);

                const auto computed_checksum = compute_checksum(packet);

                if (computed_checksum == checksum)
                {
                    this->packets.push(std::move(packet));
                }
            }

            void trim_start()
            {
                while (!this->buffer.empty() && this->buffer.front() != '$')
                {
                    buffer.erase(buffer.begin());
                }
            }
        };
    }

    bool run_gdb_stub(const network::address& bind_address)
    {
        const auto client = accept_client(bind_address);
        if (!client)
        {
            return false;
        }

        packet_queue queue{};

        while (true)
        {
            std::string packet{};
            if (!client.receive(packet))
            {
                break;
            }

            queue.enqueue(packet);
        }

        return true;
    }
}
