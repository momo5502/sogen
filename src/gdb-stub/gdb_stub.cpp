#include "gdb_stub.hpp"

#include <network/tcp_server_socket.hpp>

#include "stream_processor.hpp"
#include "checksum.hpp"

namespace gdb_stub
{
    namespace
    {
        enum class continuation_event
        {
            none,
            cont,
            detach,
            step,
        };

        bool send_packet_reply(const network::tcp_client_socket& socket, const std::string_view data)
        {
            const auto checksum = compute_checksum_as_string(data);
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

        void read_from_socket(stream_processor& processor, network::tcp_client_socket& client)
        {
            while (client.is_ready(true))
            {
                const auto data = client.receive();
                if (data)
                {
                    processor.push_stream_data(*data);
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
        stream_processor processor{};

        auto client = accept_client(bind_address);
        if (!client)
        {
            return false;
        }

        client.set_blocking(false);

        while (client.is_valid())
        {
            read_from_socket(processor, client);

            while (processor.has_packet())
            {
                const auto packet = processor.get_next_packet();
                process_packet(client, packet);
            }
        }

        return true;
    }
}
