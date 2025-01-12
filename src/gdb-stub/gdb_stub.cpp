#include "gdb_stub.hpp"

#include <cassert>
#include <queue>
#include <network/tcp_server_socket.hpp>

namespace gdb_stub
{
    namespace
    {
        constexpr size_t CHECKSUM_SIZE = 2;

        enum class continuation_event
        {
            none,
            cont,
            detach,
            step,
        };

        uint8_t compute_checksum(const std::string_view data)
        {
            uint8_t csum = 0;
            for (const auto c : data)
            {
                csum += static_cast<uint8_t>(c);
            }

            return csum;
        }

        std::string compute_checksum_string(const std::string_view data)
        {
            const auto checksum = compute_checksum(data);

            std::stringstream stream{};
            stream << std::hex << checksum;
            return stream.str();
        }

        bool send_packet_reply(const network::tcp_client_socket& socket, const std::string_view data)
        {
            const auto checksum = compute_checksum_string(data);
            return socket.send("$" + std::string(data) + "#" + checksum);
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

        void read_from_socket(packet_queue& queue, network::tcp_client_socket& client)
        {
            while (client.is_ready(true))
            {
                auto packet = client.receive();
                if (packet)
                {
                    queue.enqueue(std::move(*packet));
                }
            }
        }

        void process_query(const network::tcp_client_socket& client, const std::string_view payload)
        {
            auto name = payload;
            std::string_view args{};

            const auto separator = payload.find_first_of(':');
            if (separator != std::string_view::npos)

            {
                name = payload.substr(0, separator);
                args = payload.substr(separator + 1);
            }

            if (name == "Supported")
            {
                send_packet_reply(client, "PacketSize=1024;qXfer:features:read+");
            }
            else if (name == "Attached")
            {
                send_packet_reply(client, "1");
            }
            else if (name == "Xfer")
            {
                // process_xfer(gdbstub, args);
            }
            else if (name == "Symbol")
            {
                send_packet_reply(client, "OK");
            }
            else
            {
                send_packet_reply(client, {});
            }
        }

        continuation_event handle_command(const network::tcp_client_socket& client, const uint8_t command,
                                          const std::string_view data)
        {
            auto event = continuation_event::none;

            switch (command)
            {
            case 'q':
                process_query(client, data);
                break;

            default:
                send_packet_reply(client, {});
                break;
            }

            return event;
        }

        void process_packet(const network::tcp_client_socket& client, const std::string_view packet)
        {
            (void)client.send("+");

            if (packet.empty())
            {
                return;
            }

            const auto command = packet.front();
            const auto event = handle_command(client, command, packet.substr(1));
        }
    }

    bool run_gdb_stub(const network::address& bind_address)
    {
        packet_queue queue{};

        auto client = accept_client(bind_address);
        if (!client)
        {
            return false;
        }

        client.set_blocking(false);

        while (client.is_valid())
        {
            read_from_socket(queue, client);

            while (!queue.packets.empty())
            {
                process_packet(client, queue.packets.front());
                queue.packets.pop();
            }
        }

        return true;
    }
}
