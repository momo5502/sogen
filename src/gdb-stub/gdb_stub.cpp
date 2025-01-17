#include "gdb_stub.hpp"

#include <network/tcp_server_socket.hpp>

#include "checksum.hpp"
#include "async_handler.hpp"
#include "connection_handler.hpp"

using namespace std::literals;

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

        network::tcp_client_socket accept_client(const network::address& bind_address)
        {
            network::tcp_server_socket server{bind_address.get_family()};
            if (!server.bind(bind_address))
            {
                return false;
            }

            return server.accept();
        }

        void process_query(const connection_handler& connection, const std::string_view payload)
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
                connection.send_packet("PacketSize=1024;qXfer:features:read+");
            }
            else if (name == "Attached")
            {
                connection.send_packet("1");
            }
            else if (name == "Xfer")
            {
                // process_xfer(gdbstub, args);
            }
            else if (name == "Symbol")
            {
                connection.send_packet("OK");
            }
            else
            {
                connection.send_packet({});
            }
        }

        continuation_event handle_command(const connection_handler& connection, const uint8_t command,
                                          const std::string_view data)
        {
            auto event = continuation_event::none;

            switch (command)
            {
            case 'q':
                process_query(connection, data);
                break;

            default:
                connection.send_packet({});
                break;
            }

            return event;
        }

        void process_packet(const connection_handler& connection, const std::string_view packet)
        {
            (void)connection.send_raw_data("+");

            if (packet.empty())
            {
                return;
            }

            const auto command = packet.front();
            const auto event = handle_command(connection, command, packet.substr(1));
            (void)event;
        }

        bool is_interrupt_packet(const std::optional<std::string>& data)
        {
            return data && data->size() == 1 && data->front() == '\x03';
        }
    }

    bool run_gdb_stub(const network::address& bind_address, gdb_stub_handler& handler)
    {
        auto client = accept_client(bind_address);
        if (!client)
        {
            return false;
        }

        async_handler async{[&](std::atomic_bool& can_run) {
            while (can_run)
            {
                std::this_thread::sleep_for(10ms);

                const auto data = client.receive(1);

                if (is_interrupt_packet(data) || !client.is_valid())
                {
                    handler.on_interrupt();
                    can_run = false;
                }
            }
        }};

        connection_handler connection{client};

        while (true)
        {
            const auto packet = connection.get_packet();
            if (!packet)
            {
                break;
            }

            process_packet(connection, *packet);
        }

        return true;
    }
}
