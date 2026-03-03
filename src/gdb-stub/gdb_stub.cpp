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
        constexpr size_t max_data_size = 128 * 1024;

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

        network::tcp_client_socket accept_client(const network::address& bind_address, const utils::optional_function<bool()>& should_stop)
        {
            network::tcp_server_socket server{bind_address.get_family()};
            if (!server.bind(bind_address))
            {
                return false;
            }

            server.set_blocking(false);
            server.listen();

            while (true)
            {
                if (should_stop() || server.sleep(100ms))
                {
                    break;
                }
            }

            return server.accept();
        }

        constexpr std::string escape(const std::string_view data, size_t max_size, size_t* total_copied = nullptr)
        {
            std::string result;
            result.reserve(data.size());

            size_t count = 0;

            for (auto ch : data)
            {
                if (ch == '$' || ch == '#' || ch == '}' || ch == '*')
                {
                    if (max_size < 2)
                    {
                        break;
                    }

                    max_size--;
                    result.push_back('}');
                    ch ^= 0x20;
                }

                if (!max_size)
                {
                    break;
                }

                max_size--;
                count++;
                result.push_back(ch);
            }

            if (total_copied)
            {
                *total_copied = count;
            }

            return result;
        }

        std::string unescape(const std::string_view data)
        {
            std::string result{};
            result.reserve(data.size());

            bool xor_next = false;

            for (auto value : data)
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

            rt_assert(!xor_next);

            return result;
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
            size_t offset{};
            size_t length{};

            rt_assert(sscanf_s(args.c_str(), "%zx,%zx", &offset, &length) == 2);

            if (offset >= data.size())
            {
                connection.send_reply("l");
                return;
            }

            const auto remaining = data.size() - offset;
            const auto real_length = std::min(remaining, length);

            const auto sub_region = data.substr(offset, real_length);

            size_t num_copied = 0;
            const auto max_size = std::max(real_length, max_data_size - 1);
            const auto escaped_data = escape(sub_region, max_size, &num_copied);
            const auto is_end = num_copied == remaining;

            std::string reply = is_end ? "l" : "m";
            reply.append(escaped_data);

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

        std::string escape_xml(const std::string& str)
        {
            std::string result;
            result.reserve(str.size());
            for (char c : str)
            {
                switch (c)
                {
                case '&':
                    result += "&amp;";
                    break;
                case '<':
                    result += "&lt;";
                    break;
                case '>':
                    result += "&gt;";
                    break;
                case '"':
                    result += "&quot;";
                    break;
                default:
                    result += c;
                    break;
                }
            }
            return result;
        }

        void handle_libraries(const debugging_context& c, const std::string_view payload)
        {
            const auto [command, args] = split_string(payload, ':');

            if (command != "read")
            {
                c.connection.send_reply({});
                return;
            }

            c.handler.reset_library_stop();

            const auto [annex, data] = split_string(args, ':');
            (void)annex; // annex is empty for libraries

            std::string xml = "<library-list version=\"1.0\">\n";

            for (const auto& library : c.handler.get_libraries())
            {
                xml += "<library name=\"";
                xml += escape_xml(library.name);
                xml += "\"><segment address=\"0x";
                xml += utils::string::to_hex_number(library.segment_address);
                xml += "\"/></library>\n";
            }

            xml += "</library-list>";

            send_xfer_data(c.connection, std::string(data), xml);
        }

        void handle_exec_file(const debugging_context& c, const std::string_view payload)
        {
            const auto [command, args] = split_string(payload, ':');

            if (command != "read")
            {
                c.connection.send_reply({});
                return;
            }

            const auto [annex, data] = split_string(args, ':');
            (void)annex; // we ignore the annex

            const auto exec_path = c.handler.get_executable_path();
            send_xfer_data(c.connection, std::string(data), exec_path);
        }

        void handle_threads(const debugging_context& c, const std::string_view payload)
        {
            const auto [command, args] = split_string(payload, ':');
            if (command != "read")
            {
                c.connection.send_reply({});
                return;
            }

            const auto [annex, data] = split_string(args, ':');
            if (!annex.empty())
            {
                c.connection.send_reply({});
                return;
            }

            std::string xml = "<threads>\n";

            for (const auto& thread : c.handler.get_thread_list())
            {
                xml += "<thread id=\"";
                xml += utils::string::to_hex_number(thread.id);
                xml += "\" name=\"";
                xml += escape_xml(thread.name);
                xml += "\"/>\n";
            }
            xml += "</threads>";

            send_xfer_data(c.connection, std::string(data), xml);
        }

        void process_xfer(const debugging_context& c, const std::string_view payload)
        {
            auto [name, args] = split_string(payload, ':');

            if (name == "features")
            {
                handle_features(c, args);
            }
            else if (name == "libraries")
            {
                handle_libraries(c, args);
            }
            else if (name == "exec-file")
            {
                handle_exec_file(c, args);
            }
            else if (name == "threads")
            {
                handle_threads(c, args);
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
                std::string reply = "PacketSize=";
                reply.append(utils::string::to_hex_number(max_data_size));
                reply.append(";qXfer:features:read+"
                             ";qXfer:libraries:read+"
                             ";qXfer:exec-file:read+"
                             ";qXfer:threads:read+");

                c.connection.send_reply(reply);
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

        bool change_breakpoint(debugging_handler& handler, const bool set, const breakpoint_type type, const uint64_t address,
                               const size_t size)
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

            std::string reply = "T05";

            if (c.handler.should_signal_library())
            {
                reply += "library:;";
            }

            reply += "thread:" + hex_id + ";";

            c.connection.send_reply(reply);
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

            if (size > max_data_size / 2)
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

            if (size > max_data_size)
            {
                c.connection.send_reply("E01");
                return;
            }

            auto data = utils::string::from_hex_string(hex_data);
            data.resize(size);

            const auto res = c.handler.write_memory(address, data.data(), data.size());
            c.connection.send_reply(res ? "OK" : "E01");
        }

        void write_x_memory(const debugging_context& c, const std::string_view payload)
        {
            const auto [info, encoded_data] = split_string(payload, ':');

            size_t size{};
            uint64_t address{};
            rt_assert(sscanf_s(std::string(info).c_str(), "%" PRIx64 ",%zx", &address, &size) == 2);

            if (size > max_data_size)
            {
                c.connection.send_reply("E01");
                return;
            }

            auto data = unescape(encoded_data);
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
        const auto should_stop = [&] {
            return handler.should_stop(); //
        };

        auto client = accept_client(bind_address, should_stop);
        if (!client)
        {
            return false;
        }

        async_handler async{[&](std::atomic_bool& can_run) {
            while (can_run)
            {
                (void)client.sleep(100ms);
                const auto data = client.receive(1);

                if (is_interrupt_packet(data) || !client.is_valid() || should_stop())
                {
                    handler.on_interrupt();
                    can_run = false;
                }
            }
        }};

        debugging_state state{};
        connection_handler connection{client, should_stop};

        debugging_context c{
            .connection = connection,
            .handler = handler,
            .state = state,
            .async = async,
        };

        while (!should_stop())
        {
            const auto packet = connection.get_packet();
            if (!packet || should_stop())
            {
                break;
            }

            process_packet(c, *packet);
        }

        return true;
    }
}
