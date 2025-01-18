#include "gdb_stub.hpp"

#include <platform/compiler.hpp>
#include <network/tcp_server_socket.hpp>

#include "checksum.hpp"
#include "async_handler.hpp"
#include "connection_handler.hpp"

#include <cassert>
#include <cinttypes>

using namespace std::literals;

namespace gdb_stub
{
    namespace
    {
        void rt_assert(const bool condition)
        {
            (void)condition;
            assert(condition);
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

        std::pair<std::string_view, std::string_view> split_colon(const std::string_view payload)
        {
            auto name = payload;
            std::string_view args{};

            const auto separator = payload.find_first_of(':');
            if (separator != std::string_view::npos)

            {
                name = payload.substr(0, separator);
                args = payload.substr(separator + 1);
            }

            return {name, args};
        }

        void process_xfer(const connection_handler& connection, gdb_stub_handler& handler,
                          const std::string_view payload)
        {
            auto [name, args] = split_colon(payload);

            if (name == "features")
            {
                connection.send_reply("l<target version=\"1.0\"><architecture>" //
                                      + handler.get_target_description()        //
                                      + "<architecture>%s</architecture></target>");
            }
            else
            {
                connection.send_reply({});
            }
        }

        void process_query(const connection_handler& connection, gdb_stub_handler& handler,
                           const std::string_view payload)
        {
            auto [name, args] = split_colon(payload);

            if (name == "Supported")
            {
                connection.send_reply("PacketSize=1024;qXfer:features:read+");
            }
            else if (name == "Attached")
            {
                connection.send_reply("1");
            }
            else if (name == "Xfer")
            {
                process_xfer(connection, handler, args);
            }
            else if (name == "Symbol")
            {
                connection.send_reply("OK");
            }
            else
            {
                connection.send_reply({});
            }
        }

        void process_action(const connection_handler& connection, const gdb_action action)
        {
            if (action == gdb_action::shutdown)
            {
                connection.close();
            }
        }

        breakpoint_type translate_breakpoint_type(const uint32_t type)
        {
            if (type >= static_cast<size_t>(breakpoint_type::END))
            {
                return breakpoint_type::software;
            }

            return static_cast<breakpoint_type>(type);
        }

        bool change_breakpoint(gdb_stub_handler& handler, const bool set, const breakpoint_type type,
                               const uint64_t address, const size_t size)
        {
            if (set)
            {
                return handler.set_breakpoint(type, address, size);
            }

            return handler.delete_breakpoint(type, address, size);
        }

        void handle_breakpoint(const connection_handler& connection, gdb_stub_handler& handler, const std::string& data,
                               const bool set)
        {
            uint32_t type{};
            uint64_t addr{};
            size_t kind{};
            rt_assert(sscanf_s(data.c_str(), "%x,%" PRIX64 ",%zx", &type, &addr, &kind) == 3);

            const auto res = change_breakpoint(handler, set, translate_breakpoint_type(type), addr, kind);
            connection.send_reply(res ? "OK" : "E01");
        }

        void handle_v_packet(const connection_handler& connection, const std::string_view data)
        {
            auto [name, args] = split_colon(data);

            if (name == "Cont?")
            {
                // IDA pro gets confused if the reply arrives too early :(
                std::this_thread::sleep_for(1s);

                connection.send_reply("vCont;s;c;");
            }
            else
            {
                connection.send_reply({});
            }
        }

        void handle_command(const connection_handler& connection, async_handler& async, gdb_stub_handler& handler,
                            const uint8_t command, const std::string_view data)
        {
            switch (command)
            {
            case 'c':
                async.run();
                process_action(connection, handler.run());
                async.pause();
                break;

            case 's':
                process_action(connection, handler.singlestep());
                break;

            case 'q':
                process_query(connection, handler, data);
                break;

            case 'D':
                connection.close();
                break;

            case 'z':
            case 'Z':
                handle_breakpoint(connection, handler, std::string(data), command == 'Z');
                break;

            case '?':
                connection.send_reply("S05");
                break;

            case 'v':
                handle_v_packet(connection, data);
                break;

                // TODO
            case 'g':
            case 'm':
            case 'p':
            case 'G':
            case 'M':
            case 'P':
            case 'X':
            default:
                connection.send_reply({});
                break;
            }
        }

        void process_packet(const connection_handler& connection, async_handler& async, gdb_stub_handler& handler,
                            const std::string_view packet)
        {
            connection.send_raw_data("+");

            if (packet.empty())
            {
                return;
            }

            const auto command = packet.front();
            handle_command(connection, async, handler, command, packet.substr(1));
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

            process_packet(connection, async, handler, *packet);
        }

        return true;
    }
}
