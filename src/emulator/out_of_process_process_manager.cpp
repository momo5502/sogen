#include "out_of_process_process_manager.hpp"

#include <network/address.hpp>
#include <network/tcp_server_socket.hpp>

#ifdef poll
#undef poll
#endif

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

namespace sogen
{
    namespace
    {
        constexpr uint32_t protocol_magic = 0x50474F53;
        constexpr uint32_t protocol_version = 1;
        constexpr uint32_t maximum_payload_size = 64U << 20;

        enum class message_type : uint32_t
        {
            hello = 1,
            create_request = 2,
            started = 3,
            exited = 4,
            resume = 5,
            allocate_memory = 6,
            free_memory = 7,
            protect_memory = 8,
            read_memory = 9,
            write_memory = 10,
            response = 11,
        };

        struct message_header
        {
            uint32_t magic{};
            uint32_t version{};
            message_type type{};
            uint32_t size{};
        };

        template <typename T>
        void append_value(std::string& data, const T& value)
        {
            data.append(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        void append_string(std::string& data, const std::string& value)
        {
            append_value(data, static_cast<uint64_t>(value.size()));
            data.append(value);
        }

        template <typename T>
        T read_value(const std::string_view data, size_t& offset)
        {
            if (offset > data.size() || sizeof(T) > data.size() - offset)
            {
                throw std::runtime_error("Invalid managed process message");
            }

            T value{};
            memcpy(&value, data.data() + offset, sizeof(value));
            offset += sizeof(value);
            return value;
        }

        std::string read_string(const std::string_view data, size_t& offset)
        {
            const auto size = read_value<uint64_t>(data, offset);
            if (size > data.size() - offset || size > std::numeric_limits<size_t>::max())
            {
                throw std::runtime_error("Invalid managed process string");
            }

            std::string value{data.substr(offset, static_cast<size_t>(size))};
            offset += static_cast<size_t>(size);
            return value;
        }

        std::string serialize_request(const process_create_request& request)
        {
            std::string data{};
            append_string(data, request.application);
            append_string(data, request.working_directory);
            append_value(data, static_cast<uint64_t>(request.arguments.size()));
            for (const auto& argument : request.arguments)
            {
                append_string(data, argument);
            }
            append_value(data, static_cast<uint64_t>(request.environment.size()));
            for (const auto& [name, value] : request.environment)
            {
                append_string(data, name);
                append_string(data, value);
            }
            return data;
        }

        process_create_request deserialize_request(const std::string_view data)
        {
            constexpr uint64_t maximum_collection_size = 1ULL << 20;
            size_t offset = 0;
            process_create_request request{};
            request.application = read_string(data, offset);
            request.working_directory = read_string(data, offset);

            const auto argument_count = read_value<uint64_t>(data, offset);
            if (argument_count > maximum_collection_size)
            {
                throw std::runtime_error("Managed process request contains too many arguments");
            }
            request.arguments.reserve(static_cast<size_t>(argument_count));
            for (uint64_t i = 0; i < argument_count; ++i)
            {
                request.arguments.push_back(read_string(data, offset));
            }

            const auto environment_count = read_value<uint64_t>(data, offset);
            if (environment_count > maximum_collection_size)
            {
                throw std::runtime_error("Managed process request contains too many environment variables");
            }
            request.environment.reserve(static_cast<size_t>(environment_count));
            for (uint64_t i = 0; i < environment_count; ++i)
            {
                auto name = read_string(data, offset);
                auto value = read_string(data, offset);
                request.environment.insert_or_assign(std::move(name), std::move(value));
            }

            if (offset != data.size())
            {
                throw std::runtime_error("Invalid managed process request size");
            }
            return request;
        }

        std::optional<std::string> receive_exact(network::tcp_client_socket& socket, const size_t size)
        {
            std::string data{};
            data.reserve(size);
            while (data.size() < size)
            {
                auto part = socket.receive(size - data.size());
                if (!part)
                {
                    return std::nullopt;
                }
                data.append(*part);
            }
            return data;
        }

        std::optional<std::string> receive_exact_until(network::tcp_client_socket& socket, const size_t size,
                                                       const std::chrono::steady_clock::time_point deadline)
        {
            std::string data{};
            data.reserve(size);
            while (data.size() < size)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return std::nullopt;
                }
                if (!socket.is_ready(true))
                {
                    socket.sleep(std::chrono::milliseconds(10), true);
                    continue;
                }

                auto part = socket.receive(size - data.size());
                if (!part)
                {
                    return std::nullopt;
                }
                data.append(*part);
            }
            return data;
        }

        bool send_message(network::tcp_client_socket& socket, const message_type type, const std::string_view payload = {})
        {
            if (payload.size() > maximum_payload_size)
            {
                return false;
            }
            const message_header header{
                .magic = protocol_magic, .version = protocol_version, .type = type, .size = static_cast<uint32_t>(payload.size())};
            return socket.send(&header, sizeof(header)) && socket.send(payload);
        }

        std::optional<std::pair<message_type, std::string>> receive_message(network::tcp_client_socket& socket)
        {
            const auto header_data = receive_exact(socket, sizeof(message_header));
            if (!header_data)
            {
                return std::nullopt;
            }

            message_header header{};
            memcpy(&header, header_data->data(), sizeof(header));
            if (header.magic != protocol_magic || header.version != protocol_version || header.size > maximum_payload_size)
            {
                return std::nullopt;
            }

            auto payload = receive_exact(socket, header.size);
            if (!payload)
            {
                return std::nullopt;
            }
            return std::pair{header.type, std::move(*payload)};
        }

        std::optional<std::pair<message_type, std::string>> receive_message_until(network::tcp_client_socket& socket,
                                                                                  const std::chrono::steady_clock::time_point deadline)
        {
            const auto header_data = receive_exact_until(socket, sizeof(message_header), deadline);
            if (!header_data)
            {
                return std::nullopt;
            }

            message_header header{};
            memcpy(&header, header_data->data(), sizeof(header));
            if (header.magic != protocol_magic || header.version != protocol_version || header.size > maximum_payload_size)
            {
                return std::nullopt;
            }

            auto payload = receive_exact_until(socket, header.size, deadline);
            if (!payload)
            {
                return std::nullopt;
            }
            return std::pair{header.type, std::move(*payload)};
        }

        std::string make_token()
        {
            std::random_device random{};
            constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
            std::string token{};
            token.reserve(32);
            for (size_t i = 0; i < 4; ++i)
            {
                auto value = random();
                for (size_t digit = 0; digit < 8; ++digit)
                {
                    token.push_back(digits[value & 0xF]);
                    value >>= 4;
                }
            }
            return token;
        }

        process_error map_process_error(const std::error_code& error)
        {
            if (error == std::errc::permission_denied)
            {
                return process_error::permission_denied;
            }
            if (error == std::errc::no_such_file_or_directory)
            {
                return process_error::unavailable;
            }
            if (error == std::errc::not_enough_memory || error == std::errc::resource_unavailable_try_again)
            {
                return process_error::resource_limit;
            }
            return process_error::internal_failure;
        }
    }

    struct out_of_process_process_manager::process_entry
    {
        reproc::process process{};
        network::tcp_client_socket control{};
        std::thread output_thread{};
        mutable std::mutex mutex{};
        mutable std::optional<process_exit> exit{};
        std::optional<uint64_t> termination_code{};
        std::optional<int> host_exit_status{};
        bool host_failure{};
        std::atomic_bool kill_requested{};

        ~process_entry()
        {
            this->kill_requested = true;
            this->control.close();
            if (this->output_thread.joinable())
            {
                this->output_thread.join();
            }
        }
    };

    managed_process_connection::managed_process_connection(network::tcp_client_socket socket, process_create_request request)
        : socket_(std::move(socket)),
          request_(std::move(request))
    {
    }

    const process_create_request& managed_process_connection::request() const
    {
        return this->request_;
    }

    bool managed_process_connection::wait_for_resume(managed_process_target& target)
    {
        std::string started{};
        append_value(started, target.native_environment());
        append_value(started, target.compatibility_environment());
        append_value(started, target.native_parameters());
        append_value(started, target.compatibility_parameters());
        if (!send_message(this->socket_, message_type::started, started))
        {
            return false;
        }

        while (const auto message = receive_message(this->socket_))
        {
            if (message->first == message_type::resume)
            {
                return send_message(this->socket_, message_type::response);
            }

            size_t offset = 0;
            std::string response{};
            process_error error = process_error::internal_failure;
            try
            {
                if (message->first == message_type::allocate_memory)
                {
                    const auto address = read_value<uint64_t>(message->second, offset);
                    const auto size = read_value<uint64_t>(message->second, offset);
                    const auto permission = read_value<memory_permission>(message->second, offset);
                    const auto reserve = read_value<bool>(message->second, offset);
                    const auto commit = read_value<bool>(message->second, offset);
                    const auto result = target.allocate_memory(address, size, permission, reserve, commit);
                    append_value(response, result.error);
                    append_value(response, result.address);
                    append_value(response, result.size);
                    append_value(response, result.permission);
                }
                else if (message->first == message_type::free_memory)
                {
                    const auto address = read_value<uint64_t>(message->second, offset);
                    const auto size = read_value<uint64_t>(message->second, offset);
                    const auto release = read_value<bool>(message->second, offset);
                    append_value(response, target.free_memory(address, size, release));
                }
                else if (message->first == message_type::protect_memory)
                {
                    const auto address = read_value<uint64_t>(message->second, offset);
                    const auto size = read_value<uint64_t>(message->second, offset);
                    const auto permission = read_value<memory_permission>(message->second, offset);
                    const auto result = target.protect_memory(address, size, permission);
                    append_value(response, result.error);
                    append_value(response, result.address);
                    append_value(response, result.size);
                    append_value(response, result.permission);
                }
                else if (message->first == message_type::read_memory)
                {
                    const auto address = read_value<uint64_t>(message->second, offset);
                    const auto size = read_value<uint64_t>(message->second, offset);
                    const auto result = target.read_memory(address, size);
                    append_value(response, result.error);
                    append_value(response, static_cast<uint64_t>(result.data.size()));
                    response.append(reinterpret_cast<const char*>(result.data.data()), result.data.size());
                }
                else if (message->first == message_type::write_memory)
                {
                    const auto address = read_value<uint64_t>(message->second, offset);
                    const auto size = read_value<uint64_t>(message->second, offset);
                    if (size > message->second.size() - offset)
                    {
                        throw std::runtime_error("Invalid managed process memory write");
                    }
                    const auto data =
                        std::span{reinterpret_cast<const uint8_t*>(message->second.data() + offset), static_cast<size_t>(size)};
                    const auto result = target.write_memory(address, data);
                    append_value(response, result.error);
                    append_value(response, result.address);
                    append_value(response, result.size);
                    append_value(response, result.permission);
                }
                else
                {
                    append_value(response, error);
                }
            }
            catch (...)
            {
                response.clear();
                append_value(response, error);
            }

            if (!send_message(this->socket_, message_type::response, response))
            {
                return false;
            }
        }
        return false;
    }

    bool managed_process_connection::notify_exit(const uint64_t exit_code)
    {
        std::string payload{};
        append_value(payload, exit_code);
        return send_message(this->socket_, message_type::exited, payload);
    }

    managed_process_connection connect_managed_process(const uint16_t port, const std::string& token)
    {
        network::tcp_client_socket socket{AF_INET};
        if (!socket.connect(network::address{"127.0.0.1", port}) || !send_message(socket, message_type::hello, token))
        {
            throw std::runtime_error("Connecting to the managed process parent failed");
        }

        const auto message = receive_message(socket);
        if (!message || message->first != message_type::create_request)
        {
            throw std::runtime_error("Receiving the managed process request failed");
        }
        return {std::move(socket), deserialize_request(message->second)};
    }

    out_of_process_process_manager::out_of_process_process_manager(std::filesystem::path executable, argument_factory arguments)
        : executable_(std::move(executable)),
          arguments_(std::move(arguments))
    {
        if (this->executable_.empty() || !this->arguments_)
        {
            throw std::invalid_argument("An executable and argument factory are required");
        }
    }

    out_of_process_process_manager::~out_of_process_process_manager()
    {
        std::vector<std::shared_ptr<process_entry>> processes{};
        {
            const std::scoped_lock lock(this->mutex_);
            this->stopping_ = true;
            processes.reserve(this->processes_.size());
            for (const auto& [handle, process] : this->processes_)
            {
                (void)handle;
                processes.push_back(process);
            }
        }

        for (const auto& process : processes)
        {
            process->kill_requested = true;
        }
    }

    process_create_result out_of_process_process_manager::create_process(process_create_request request)
    {
        managed_process handle{};
        {
            const std::scoped_lock lock(this->mutex_);
            if (this->stopping_)
            {
                return {.error = process_error::unavailable};
            }
            if (this->next_process_ == 0)
            {
                return {.error = process_error::resource_limit};
            }
            handle.value = this->next_process_++;
        }

        network::tcp_server_socket server{AF_INET};
        if (!server.bind(network::address{"127.0.0.1", 0}))
        {
            return {.error = process_error::communication_failure};
        }
        server.listen();

        const auto token = make_token();
        std::vector<std::string> arguments{};
        try
        {
            arguments = this->arguments_(server.get_port(), token);
        }
        catch (...)
        {
            return {.error = process_error::internal_failure};
        }

        std::vector<std::string> command{};
        command.push_back(this->executable_.string());
        for (auto& argument : arguments)
        {
            command.push_back(std::move(argument));
        }

        const auto entry = std::make_shared<process_entry>();
        reproc::options options{};
        options.redirect.in.type = reproc::redirect::parent;
        options.redirect.out.type = reproc::redirect::pipe;
        options.redirect.err.type = reproc::redirect::pipe;
        options.stop = {.first = {.action = reproc::stop::terminate, .timeout = reproc::milliseconds(2000)},
                        .second = {.action = reproc::stop::kill, .timeout = reproc::milliseconds(2000)},
                        .third = {.action = reproc::stop::wait, .timeout = reproc::milliseconds(1000)}};

        const auto start_error = entry->process.start(command, options);
        if (start_error)
        {
            return {.error = map_process_error(start_error)};
        }

        entry->output_thread = std::thread([process = entry.get()] {
            std::array<uint8_t, 4096> buffer{};
            for (;;)
            {
                if (process->kill_requested.exchange(false))
                {
                    if (process->process.kill())
                    {
                        const std::scoped_lock lock(process->mutex);
                        process->host_failure = true;
                        break;
                    }
                }
                const auto [events, poll_error] =
                    process->process.poll(reproc::event::out | reproc::event::err | reproc::event::exit, reproc::milliseconds(50));
                if (poll_error && poll_error != std::errc::timed_out)
                {
                    const std::scoped_lock lock(process->mutex);
                    process->host_failure = true;
                    break;
                }

                for (const auto& [event, stream, file] : {std::tuple{reproc::event::out, reproc::stream::out, stdout},
                                                          std::tuple{reproc::event::err, reproc::stream::err, stderr}})
                {
                    if ((events & event) == 0)
                    {
                        continue;
                    }
                    const auto [size, read_error] = process->process.read(stream, buffer.data(), buffer.size());
                    if (!read_error)
                    {
                        (void)fwrite(buffer.data(), 1, size, file);
                        fflush(file);
                    }
                }

                if ((events & reproc::event::exit) != 0)
                {
                    const auto [status, wait_error] = process->process.wait(reproc::infinite);
                    const std::scoped_lock lock(process->mutex);
                    if (wait_error)
                    {
                        process->host_failure = true;
                    }
                    else
                    {
                        process->host_exit_status = status;
                    }
                    break;
                }
            }
        });

        constexpr auto connection_timeout = std::chrono::seconds(30);
        const auto deadline = std::chrono::steady_clock::now() + connection_timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!server.is_ready(true))
            {
                server.sleep(std::chrono::milliseconds(10), true);
                continue;
            }

            auto candidate = server.accept();
            if (!candidate.set_blocking(false))
            {
                continue;
            }

            const auto candidate_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
            auto hello = receive_message_until(candidate, candidate_deadline);
            if (hello && hello->first == message_type::hello && hello->second == token)
            {
                entry->control = std::move(candidate);
                break;
            }
        }

        if (!entry->control || !send_message(entry->control, message_type::create_request, serialize_request(request)))
        {
            return {.error = process_error::communication_failure};
        }

        const auto started = receive_message_until(entry->control, deadline);
        if (!started || started->first != message_type::started)
        {
            return {.error = process_error::internal_failure};
        }
        if (!entry->control.set_blocking(true))
        {
            return {.error = process_error::communication_failure};
        }

        {
            const std::scoped_lock lock(this->mutex_);
            if (this->stopping_)
            {
                return {.error = process_error::unavailable};
            }
            this->processes_.emplace(handle.value, entry);
        }
        size_t offset = 0;
        const auto native_environment = read_value<uint64_t>(started->second, offset);
        const auto compatibility_environment = read_value<uint64_t>(started->second, offset);
        const auto native_parameters = read_value<uint64_t>(started->second, offset);
        const auto compatibility_parameters = read_value<uint64_t>(started->second, offset);
        if (offset != started->second.size())
        {
            return {.error = process_error::communication_failure};
        }
        return {.process = handle,
                .native_environment = native_environment,
                .compatibility_environment = compatibility_environment,
                .native_parameters = native_parameters,
                .compatibility_parameters = compatibility_parameters};
    }

    process_error out_of_process_process_manager::resume_process(const managed_process process)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::resume))
        {
            return process_error::communication_failure;
        }
        const auto response = receive_message(entry->control);
        return response && response->first == message_type::response ? process_error::none : process_error::communication_failure;
    }

    process_memory_result out_of_process_process_manager::allocate_memory(const managed_process process, const uint64_t address,
                                                                          const uint64_t size, const memory_permission permission,
                                                                          const bool reserve, const bool commit)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        std::string payload{};
        append_value(payload, address);
        append_value(payload, size);
        append_value(payload, permission);
        append_value(payload, reserve);
        append_value(payload, commit);
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::allocate_memory, payload))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = receive_message(entry->control);
        if (!response || response->first != message_type::response)
        {
            return {.error = process_error::communication_failure};
        }
        try
        {
            size_t offset = 0;
            process_memory_result result{};
            result.error = read_value<process_error>(response->second, offset);
            result.address = read_value<uint64_t>(response->second, offset);
            result.size = read_value<uint64_t>(response->second, offset);
            result.permission = read_value<memory_permission>(response->second, offset);
            return offset == response->second.size() ? result : process_memory_result{.error = process_error::communication_failure};
        }
        catch (...)
        {
            return {.error = process_error::communication_failure};
        }
    }

    process_error out_of_process_process_manager::free_memory(const managed_process process, const uint64_t address, const uint64_t size,
                                                              const bool release)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }
        std::string payload{};
        append_value(payload, address);
        append_value(payload, size);
        append_value(payload, release);
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::free_memory, payload))
        {
            return process_error::communication_failure;
        }
        const auto response = receive_message(entry->control);
        if (!response || response->first != message_type::response)
        {
            return process_error::communication_failure;
        }
        try
        {
            size_t offset = 0;
            const auto error = read_value<process_error>(response->second, offset);
            return offset == response->second.size() ? error : process_error::communication_failure;
        }
        catch (...)
        {
            return process_error::communication_failure;
        }
    }

    process_memory_result out_of_process_process_manager::protect_memory(const managed_process process, const uint64_t address,
                                                                         const uint64_t size, const memory_permission permission)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        std::string payload{};
        append_value(payload, address);
        append_value(payload, size);
        append_value(payload, permission);
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::protect_memory, payload))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = receive_message(entry->control);
        if (!response || response->first != message_type::response)
        {
            return {.error = process_error::communication_failure};
        }
        try
        {
            size_t offset = 0;
            process_memory_result result{};
            result.error = read_value<process_error>(response->second, offset);
            result.address = read_value<uint64_t>(response->second, offset);
            result.size = read_value<uint64_t>(response->second, offset);
            result.permission = read_value<memory_permission>(response->second, offset);
            return offset == response->second.size() ? result : process_memory_result{.error = process_error::communication_failure};
        }
        catch (...)
        {
            return {.error = process_error::communication_failure};
        }
    }

    process_memory_read_result out_of_process_process_manager::read_memory(const managed_process process, const uint64_t address,
                                                                           const uint64_t size) const
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        std::string payload{};
        append_value(payload, address);
        append_value(payload, size);
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::read_memory, payload))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = receive_message(entry->control);
        if (!response || response->first != message_type::response)
        {
            return {.error = process_error::communication_failure};
        }
        try
        {
            size_t offset = 0;
            process_memory_read_result result{};
            result.error = read_value<process_error>(response->second, offset);
            const auto data_size = read_value<uint64_t>(response->second, offset);
            if (data_size > response->second.size() - offset)
            {
                return {.error = process_error::communication_failure};
            }
            result.data.assign(response->second.begin() + static_cast<ptrdiff_t>(offset), response->second.end());
            return result;
        }
        catch (...)
        {
            return {.error = process_error::communication_failure};
        }
    }

    process_memory_result out_of_process_process_manager::write_memory(const managed_process process, const uint64_t address,
                                                                       const std::span<const uint8_t> data)
    {
        const auto entry = this->find_process(process);
        if (!entry || data.size() > maximum_payload_size - sizeof(address) - sizeof(uint64_t))
        {
            return {.error = entry ? process_error::resource_limit : process_error::invalid_process};
        }
        std::string payload{};
        append_value(payload, address);
        append_value(payload, static_cast<uint64_t>(data.size()));
        payload.append(reinterpret_cast<const char*>(data.data()), data.size());
        const std::scoped_lock lock(entry->mutex);
        if (!send_message(entry->control, message_type::write_memory, payload))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = receive_message(entry->control);
        if (!response || response->first != message_type::response)
        {
            return {.error = process_error::communication_failure};
        }
        try
        {
            size_t offset = 0;
            process_memory_result result{};
            result.error = read_value<process_error>(response->second, offset);
            result.address = read_value<uint64_t>(response->second, offset);
            result.size = read_value<uint64_t>(response->second, offset);
            result.permission = read_value<memory_permission>(response->second, offset);
            return offset == response->second.size() ? result : process_memory_result{.error = process_error::communication_failure};
        }
        catch (...)
        {
            return {.error = process_error::communication_failure};
        }
    }

    process_error out_of_process_process_manager::terminate_process(const managed_process process, const uint64_t exit_code)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }

        const std::scoped_lock lock(entry->mutex);
        if (entry->exit)
        {
            return process_error::none;
        }
        entry->termination_code = exit_code;
        entry->kill_requested = true;
        return process_error::none;
    }

    process_exit_status_result out_of_process_process_manager::exit_status(const managed_process process) const
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }

        const std::scoped_lock lock(entry->mutex);
        if (entry->exit)
        {
            return {.status = entry->exit};
        }

        if (!entry->host_exit_status && !entry->host_failure)
        {
            return {};
        }

        if (entry->host_exit_status)
        {
            const auto message = receive_message(entry->control);
            if (message && message->first == message_type::exited)
            {
                size_t offset = 0;
                const auto exit_code = read_value<uint64_t>(message->second, offset);
                if (offset == message->second.size())
                {
                    entry->exit = process_exit{.kind = process_exit_kind::exited, .code = exit_code};
                    return {.status = entry->exit};
                }
            }
        }

        if (entry->termination_code)
        {
            entry->exit = process_exit{.kind = process_exit_kind::terminated, .code = *entry->termination_code};
            return {.status = entry->exit};
        }

        entry->exit = process_exit{.kind = process_exit_kind::runtime_failure,
                                   .code = entry->host_exit_status ? static_cast<uint32_t>(*entry->host_exit_status) : 1};
        return {.status = entry->exit};
    }

    std::shared_ptr<out_of_process_process_manager::process_entry> out_of_process_process_manager::find_process(
        const managed_process process) const
    {
        const std::scoped_lock lock(this->mutex_);
        const auto entry = this->processes_.find(process.value);
        return entry == this->processes_.end() ? nullptr : entry->second;
    }
}
