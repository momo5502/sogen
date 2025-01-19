#include "gdb_stub.hpp"

#include <cassert>
#include <cinttypes>

#include <utils/string.hpp>
#include <platform/compiler.hpp>
#include <network/tcp_server_socket.hpp>

#include "async_handler.hpp"
#include "connection_handler.hpp"

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

        std::pair<std::string_view, std::string_view> split_string(const std::string_view payload, const char separator)
        {
            auto name = payload;
            std::string_view args{};

            const auto separator_pos = payload.find_first_of(separator);
            if (separator_pos != std::string_view::npos)

            {
                name = payload.substr(0, separator_pos);
                args = payload.substr(separator_pos + 1);
            }

            return {name, args};
        }

        void handle_features(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            const auto [command, args] = split_string(payload, ':');

            if (command != "read")
            {
                connection.send_reply({});
                return;
            }

            const auto [file, data] = split_string(args, ':');

            size_t offset{}, length{};
            rt_assert(sscanf_s(std::string(data).c_str(), "%zx,%zx", &offset, &length) == 2);

            const auto target_description = handler.get_target_description(file);

            if (offset >= target_description.size())
            {
                connection.send_reply("l");
                return;
            }

            const auto remaining = target_description.size() - offset;
            const auto real_length = std::min(remaining, length);
            const auto is_end = real_length == remaining;

            const auto sub_region = target_description.substr(offset, real_length);

            connection.send_reply((is_end ? "l" : "m") + sub_region);
        }

        void process_xfer(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            auto [name, args] = split_string(payload, ':');

            if (name == "features")
            {
                handle_features(connection, handler, args);
            }
            else
            {
                connection.send_reply({});
            }
        }

        void process_query(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            const auto [name, args] = split_string(payload, ':');

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

        void process_action(connection_handler& connection, const action a)
        {
            if (a == action::shutdown)
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

        bool change_breakpoint(debugging_handler& handler, const bool set, const breakpoint_type type,
                               const uint64_t address, const size_t size)
        {
            if (set)
            {
                return handler.set_breakpoint(type, address, size);
            }

            return handler.delete_breakpoint(type, address, size);
        }

        void handle_breakpoint(connection_handler& connection, debugging_handler& handler, const std::string& data,
                               const bool set)
        {
            uint32_t type{};
            uint64_t addr{};
            size_t kind{};
            rt_assert(sscanf_s(data.c_str(), "%x,%" PRIX64 ",%zx", &type, &addr, &kind) == 3);

            const auto res = change_breakpoint(handler, set, translate_breakpoint_type(type), addr, kind);
            connection.send_reply(res ? "OK" : "E01");
        }

        void continue_execution(connection_handler& connection, async_handler& async, debugging_handler& handler)
        {
            async.run();
            process_action(connection, handler.run());
            async.pause();
            connection.send_reply("S05");
        }

        void singlestep_execution(connection_handler& connection, debugging_handler& handler)
        {
            process_action(connection, handler.singlestep());
            connection.send_reply("S05");
        }

        void handle_v_packet(connection_handler& connection, async_handler& async, debugging_handler& handler,
                             const std::string_view data)
        {
            const auto [name, args] = split_string(data, ':');

            if (name == "Cont?")
            {
                connection.send_reply("vCont;s;c");
            }
            else if (name == "Cont;s")
            {
                singlestep_execution(connection, handler);
            }
            else if (name == "Cont;c")
            {
                continue_execution(connection, async, handler);
            }
            else
            {
                connection.send_reply({});
            }
        }

        void read_registers(connection_handler& connection, debugging_handler& handler)
        {
            std::string response{};
            std::vector<std::byte> data{};
            data.resize(handler.get_max_register_size());

            const auto registers = handler.get_register_count();

            for (size_t i = 0; i < registers; ++i)
            {
                const auto size = handler.read_register(i, data.data(), data.size());

                if (!size)
                {
                    connection.send_reply("E01");
                    return;
                }

                const std::span register_data(data.data(), size);
                response.append(utils::string::to_hex_string(register_data));
            }

            connection.send_reply(response);
        }

        void write_registers(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            const auto data = utils::string::from_hex_string(payload);

            const auto registers = handler.get_register_count();
            const auto register_size = handler.get_max_register_size();

            size_t offset = 0;
            for (size_t i = 0; i < registers; ++i)
            {
                if (offset >= data.size())
                {
                    connection.send_reply("E01");
                    return;
                }

                const auto max_size = std::min(register_size, data.size() - offset);
                const auto size = handler.write_register(i, data.data() + offset, max_size);

                offset += size;

                if (!size)
                {
                    connection.send_reply("E01");
                    return;
                }
            }

            connection.send_reply("OK");
        }

        void read_single_register(connection_handler& connection, debugging_handler& handler,
                                  const std::string& payload)
        {
            size_t reg{};
            rt_assert(sscanf_s(payload.c_str(), "%zx", &reg) == 1);

            std::vector<std::byte> data{};
            data.resize(handler.get_max_register_size());

            const auto size = handler.read_register(reg, data.data(), data.size());

            if (size)
            {
                const std::span register_data(data.data(), size);
                connection.send_reply(utils::string::to_hex_string(register_data));
            }
            else
            {
                connection.send_reply("E01");
            }
        }

        void write_single_register(connection_handler& connection, debugging_handler& handler,
                                   const std::string_view payload)
        {
            const auto [reg, hex_data] = split_string(payload, '=');

            size_t register_index{};
            rt_assert(sscanf_s(std::string(reg).c_str(), "%zx", &register_index) == 1);

            const auto data = utils::string::from_hex_string(hex_data);
            const auto res = handler.write_register(register_index, data.data(), data.size()) > 0;
            connection.send_reply(res ? "OK" : "E01");
        }

        void read_memory(connection_handler& connection, debugging_handler& handler, const std::string& payload)
        {
            uint64_t address{};
            size_t size{};
            rt_assert(sscanf_s(payload.c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                connection.send_reply("E01");
                return;
            }

            std::vector<std::byte> data{};
            data.resize(size);

            const auto res = handler.read_memory(address, data.data(), data.size());
            if (!res)
            {
                connection.send_reply("E01");
                return;
            }

            connection.send_reply(utils::string::to_hex_string(data));
        }

        void write_memory(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            const auto [info, hex_data] = split_string(payload, ':');

            size_t size{};
            uint64_t address{};
            rt_assert(sscanf_s(std::string(info).c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                connection.send_reply("E01");
                return;
            }

            auto data = utils::string::from_hex_string(hex_data);
            data.resize(size);

            const auto res = handler.write_memory(address, data.data(), data.size());
            connection.send_reply(res ? "OK" : "E01");
        }

        std::string decode_x_memory(const std::string_view payload)
        {
            std::string result{};
            result.reserve(payload.size());

            bool xor_next = false;

            for (auto value : payload)
            {
                if (xor_next)
                {
                    value ^= 0x20;
                    xor_next = false;
                }
                else if (value == '}')
                {
                    xor_next = true;
                    continue;
                }

                result.push_back(value);
            }

            return result;
        }

        void write_x_memory(connection_handler& connection, debugging_handler& handler, const std::string_view payload)
        {
            const auto [info, encoded_data] = split_string(payload, ':');

            size_t size{};
            uint64_t address{};
            rt_assert(sscanf_s(std::string(info).c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                connection.send_reply("E01");
                return;
            }

            auto data = decode_x_memory(encoded_data);
            data.resize(size);

            const auto res = handler.write_memory(address, data.data(), data.size());
            if (!res)
            {
                connection.send_reply("E01");
                return;
            }

            connection.send_reply("OK");
        }

        void handle_command(connection_handler& connection, async_handler& async, debugging_handler& handler,
                            const uint8_t command, const std::string_view data)
        {
            // printf("GDB command: %c -> %.*s\n", command, static_cast<int>(data.size()), data.data());

            switch (command)
            {
            case 'c':
                continue_execution(connection, async, handler);
                break;

            case 's':
                singlestep_execution(connection, handler);
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
                handle_v_packet(connection, async, handler, data);
                break;

            case 'g':
                read_registers(connection, handler);
                break;

            case 'G':
                write_registers(connection, handler, data);
                break;

            case 'p':
                read_single_register(connection, handler, std::string(data));
                break;

            case 'P':
                write_single_register(connection, handler, data);
                break;

            case 'm':
                read_memory(connection, handler, std::string(data));
                break;

            case 'M':
                write_memory(connection, handler, data);
                break;

            case 'X':
                write_x_memory(connection, handler, data);
                break;

            default:
                connection.send_reply({});
                break;
            }
        }

        void process_packet(connection_handler& connection, async_handler& async, debugging_handler& handler,
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

    bool run_gdb_stub(const network::address& bind_address, debugging_handler& handler)
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
