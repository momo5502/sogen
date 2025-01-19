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

        struct debugging_state
        {
            std::optional<uint32_t> continuation_thread{};
        };

        struct debugging_context
        {
            connection_handler& connection;
            debugging_handler& handler;
            debugging_state& state;
            async_handler& async;
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

        void send_xfer_data(connection_handler& connection, const std::string& args, const std::string_view data)
        {
            size_t offset{}, length{};
            rt_assert(sscanf_s(args.c_str(), "%zx,%zx", &offset, &length) == 2);

            if (offset >= data.size())
            {
                connection.send_reply("l");
                return;
            }

            const auto remaining = data.size() - offset;
            const auto real_length = std::min(remaining, length);
            const auto is_end = real_length == remaining;

            const auto sub_region = data.substr(offset, real_length);

            std::string reply = is_end ? "l" : "m";
            reply.append(sub_region);

            connection.send_reply(reply);
        }

        void handle_features(const debugging_context& c, const std::string_view payload)
        {
            const auto [command, args] = split_string(payload, ':');

            if (command != "read")
            {
                c.connection.send_reply({});
                return;
            }

            const auto [file, data] = split_string(args, ':');
            const auto target_description = c.handler.get_target_description(file);
            send_xfer_data(c.connection, std::string(data), target_description);
        }

        void process_xfer(const debugging_context& c, const std::string_view payload)
        {
            auto [name, args] = split_string(payload, ':');

            if (name == "features")
            {
                handle_features(c, args);
            }
            else
            {
                c.connection.send_reply({});
            }
        }

        void process_query(const debugging_context& c, const std::string_view payload)
        {
            const auto [name, args] = split_string(payload, ':');

            if (name == "Supported")
            {
                c.connection.send_reply("PacketSize=1024;qXfer:features:read+");
            }
            else if (name == "Attached")
            {
                c.connection.send_reply("1");
            }
            else if (name == "Xfer")
            {
                process_xfer(c, args);
            }
            else if (name == "Symbol")
            {
                c.connection.send_reply("OK");
            }
            else if (name == "C")
            {
                const auto thread_id = c.handler.get_current_thread_id();
                c.connection.send_reply("QC" + utils::string::to_hex_number(thread_id));
            }
            else if (name == "sThreadInfo")
            {
                c.connection.send_reply("l");
            }
            else if (name == "fThreadInfo")
            {
                std::string reply{};
                const auto ids = c.handler.get_thread_ids();

                for (const auto id : ids)
                {
                    reply.push_back(reply.empty() ? 'm' : ',');
                    reply.append(utils::string::to_hex_number(id));
                }

                c.connection.send_reply(reply);
            }
            else
            {
                c.connection.send_reply({});
            }
        }

        void process_action(const connection_handler& connection, const action a)
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

        void handle_breakpoint(const debugging_context& c, const std::string& data, const bool set)
        {
            uint32_t type{};
            uint64_t addr{};
            size_t kind{};
            rt_assert(sscanf_s(data.c_str(), "%x,%" PRIX64 ",%zx", &type, &addr, &kind) == 3);

            const auto res = change_breakpoint(c.handler, set, translate_breakpoint_type(type), addr, kind);
            c.connection.send_reply(res ? "OK" : "E01");
        }

        void signal_stop(const debugging_context& c)
        {
            const auto exit_status = c.handler.get_exit_code();
            if (exit_status)
            {
                c.connection.send_reply(*exit_status == 0 ? "W00" : "WFF");
                return;
            }

            const auto id = c.handler.get_current_thread_id();
            const auto hex_id = utils::string::to_hex_number(id);
            c.connection.send_reply("T05thread:" + hex_id + ";");
        }

        void apply_continuation_thread(const debugging_context& c)
        {
            if (c.state.continuation_thread)
            {
                c.handler.switch_to_thread(*c.state.continuation_thread);
                c.state.continuation_thread = std::nullopt;
            }
        }

        void resume_execution(const debugging_context& c, const bool single_step)
        {
            apply_continuation_thread(c);

            action a{};

            if (single_step)
            {
                a = c.handler.singlestep();
            }
            else
            {
                c.async.run();
                a = c.handler.run();
                c.async.pause();
            }

            process_action(c.connection, a);
            signal_stop(c);
        }

        void store_continuation_thread(const debugging_context& c, const std::string_view thread_string)
        {
            if (thread_string.empty())
            {
                return;
            }

            uint32_t thread_id{};
            rt_assert(sscanf_s(std::string(thread_string).c_str(), "%x", &thread_id) == 1);
            c.state.continuation_thread = thread_id;
        }

        void handle_v_packet(const debugging_context& c, const std::string_view data)
        {
            const auto [name, args] = split_string(data, ':');

            if (name == "Cont?")
            {
                c.connection.send_reply("vCont;s;c");
            }
            else if (name == "Cont;s" || name == "Cont;c")
            {
                const auto singlestep = name[5] == 's';
                const auto [thread, _] = split_string(args, ':');

                store_continuation_thread(c, thread);
                resume_execution(c, singlestep);
            }
            else
            {
                c.connection.send_reply({});
            }
        }

        void read_registers(const debugging_context& c)
        {
            std::string response{};
            std::vector<std::byte> data{};
            data.resize(c.handler.get_max_register_size());

            const auto registers = c.handler.get_register_count();

            for (size_t i = 0; i < registers; ++i)
            {
                const auto size = c.handler.read_register(i, data.data(), data.size());

                if (!size)
                {
                    c.connection.send_reply("E01");
                    return;
                }

                const std::span register_data(data.data(), size);
                response.append(utils::string::to_hex_string(register_data));
            }

            c.connection.send_reply(response);
        }

        void write_registers(const debugging_context& c, const std::string_view payload)
        {
            const auto data = utils::string::from_hex_string(payload);

            const auto registers = c.handler.get_register_count();
            const auto register_size = c.handler.get_max_register_size();

            size_t offset = 0;
            for (size_t i = 0; i < registers; ++i)
            {
                if (offset >= data.size())
                {
                    c.connection.send_reply("E01");
                    return;
                }

                const auto max_size = std::min(register_size, data.size() - offset);
                const auto size = c.handler.write_register(i, data.data() + offset, max_size);

                offset += size;

                if (!size)
                {
                    c.connection.send_reply("E01");
                    return;
                }
            }

            c.connection.send_reply("OK");
        }

        void read_single_register(const debugging_context& c, const std::string& payload)
        {
            size_t reg{};
            rt_assert(sscanf_s(payload.c_str(), "%zx", &reg) == 1);

            std::vector<std::byte> data{};
            data.resize(c.handler.get_max_register_size());

            const auto size = c.handler.read_register(reg, data.data(), data.size());

            if (size)
            {
                const std::span register_data(data.data(), size);
                c.connection.send_reply(utils::string::to_hex_string(register_data));
            }
            else
            {
                c.connection.send_reply("E01");
            }
        }

        void write_single_register(const debugging_context& c, const std::string_view payload)
        {
            const auto [reg, hex_data] = split_string(payload, '=');

            size_t register_index{};
            rt_assert(sscanf_s(std::string(reg).c_str(), "%zx", &register_index) == 1);

            const auto data = utils::string::from_hex_string(hex_data);
            const auto res = c.handler.write_register(register_index, data.data(), data.size()) > 0;
            c.connection.send_reply(res ? "OK" : "E01");
        }

        void read_memory(const debugging_context& c, const std::string& payload)
        {
            uint64_t address{};
            size_t size{};
            rt_assert(sscanf_s(payload.c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                c.connection.send_reply("E01");
                return;
            }

            std::vector<std::byte> data{};
            data.resize(size);

            const auto res = c.handler.read_memory(address, data.data(), data.size());
            if (!res)
            {
                c.connection.send_reply("E01");
                return;
            }

            c.connection.send_reply(utils::string::to_hex_string(data));
        }

        void write_memory(const debugging_context& c, const std::string_view payload)
        {
            const auto [info, hex_data] = split_string(payload, ':');

            size_t size{};
            uint64_t address{};
            rt_assert(sscanf_s(std::string(info).c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                c.connection.send_reply("E01");
                return;
            }

            auto data = utils::string::from_hex_string(hex_data);
            data.resize(size);

            const auto res = c.handler.write_memory(address, data.data(), data.size());
            c.connection.send_reply(res ? "OK" : "E01");
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

        void write_x_memory(const debugging_context& c, const std::string_view payload)
        {
            const auto [info, encoded_data] = split_string(payload, ':');

            size_t size{};
            uint64_t address{};
            rt_assert(sscanf_s(std::string(info).c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > 0x1000)
            {
                c.connection.send_reply("E01");
                return;
            }

            auto data = decode_x_memory(encoded_data);
            data.resize(size);

            const auto res = c.handler.write_memory(address, data.data(), data.size());
            if (!res)
            {
                c.connection.send_reply("E01");
                return;
            }

            c.connection.send_reply("OK");
        }

        void switch_to_thread(const debugging_context& c, const std::string_view payload)
        {
            if (payload.size() < 2)
            {
                c.connection.send_reply({});
                return;
            }

            uint32_t id{};
            rt_assert(sscanf_s(std::string(payload.substr(1)).c_str(), "%x", &id) == 1);

            const auto operation = payload[0];
            if (operation == 'c')
            {
                c.state.continuation_thread = id;
                c.connection.send_reply("OK");
            }
            else if (operation == 'g')
            {
                const auto res = id == 0 || c.handler.switch_to_thread(id);
                c.connection.send_reply(res ? "OK" : "E01");
            }
            else
            {
                c.connection.send_reply({});
            }
        }

        void handle_command(const debugging_context& c, const uint8_t command, const std::string_view data)
        {
            // printf("GDB command: %c -> %.*s\n", command, static_cast<int>(data.size()), data.data());

            switch (command)
            {
            case 'S':
            case 'c':
                resume_execution(c, false);
                break;

            case 's':
                resume_execution(c, true);
                break;

            case 'q':
                process_query(c, data);
                break;

            case 'D':
                c.connection.close();
                break;

            case 'z':
            case 'Z':
                handle_breakpoint(c, std::string(data), command == 'Z');
                break;

            case '?':
                signal_stop(c);
                break;

            case 'v':
                handle_v_packet(c, data);
                break;

            case 'g':
                read_registers(c);
                break;

            case 'G':
                write_registers(c, data);
                break;

            case 'p':
                read_single_register(c, std::string(data));
                break;

            case 'P':
                write_single_register(c, data);
                break;

            case 'm':
                read_memory(c, std::string(data));
                break;

            case 'M':
                write_memory(c, data);
                break;

            case 'X':
                write_x_memory(c, data);
                break;

            case 'H':
                switch_to_thread(c, data);
                break;

            default:
                c.connection.send_reply({});
                break;
            }
        }

        void process_packet(const debugging_context& c, const std::string_view packet)
        {
            c.connection.send_raw_data("+");

            if (packet.empty())
            {
                return;
            }

            const auto command = packet.front();
            handle_command(c, command, packet.substr(1));
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

        debugging_state state{};
        connection_handler connection{client};

        debugging_context c{
            .connection = connection,
            .handler = handler,
            .state = state,
            .async = async,
        };

        while (true)
        {
            const auto packet = connection.get_packet();
            if (!packet)
            {
                break;
            }

            process_packet(c, *packet);
        }

        return true;
    }
}
