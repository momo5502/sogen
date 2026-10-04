#include "subprocess_process_manager.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include "process_protocol_generated.hxx"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

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
#include <cstdio>
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
        constexpr uint32_t maximum_payload_size = 64U << 20;
        constexpr auto response_timeout = std::chrono::seconds(5);
        constexpr auto exit_notification_timeout = std::chrono::milliseconds(250);

        template <typename T>
        bool send_message(network::tcp_client_socket& socket, T payload)
        {
            ProcessProtocol::MessageT message{};
            message.payload.Set(std::move(payload));

            flatbuffers::FlatBufferBuilder builder{};
            ProcessProtocol::FinishSizePrefixedMessageBuffer(builder, ProcessProtocol::Message::Pack(builder, &message));
            if (builder.GetSize() > maximum_payload_size + sizeof(flatbuffers::uoffset_t))
            {
                return false;
            }
            return socket.send(builder.GetBufferPointer(), builder.GetSize());
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

        std::optional<ProcessProtocol::MessageT> decode_message(std::string data)
        {
            flatbuffers::Verifier verifier{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
            if (!ProcessProtocol::VerifySizePrefixedMessageBuffer(verifier))
            {
                return std::nullopt;
            }

            ProcessProtocol::MessageT message{};
            ProcessProtocol::GetSizePrefixedMessage(data.data())->UnPackTo(&message);
            return message;
        }

        std::optional<ProcessProtocol::MessageT> receive_message(network::tcp_client_socket& socket)
        {
            auto size_data = receive_exact(socket, sizeof(flatbuffers::uoffset_t));
            if (!size_data)
            {
                return std::nullopt;
            }

            const auto size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(size_data->data());
            if (size > maximum_payload_size)
            {
                return std::nullopt;
            }

            auto data = receive_exact(socket, size);
            if (!data)
            {
                return std::nullopt;
            }
            size_data->append(*data);
            return decode_message(std::move(*size_data));
        }

        std::optional<ProcessProtocol::MessageT> receive_message_until(network::tcp_client_socket& socket,
                                                                       const std::chrono::steady_clock::time_point deadline)
        {
            auto size_data = receive_exact_until(socket, sizeof(flatbuffers::uoffset_t), deadline);
            if (!size_data)
            {
                return std::nullopt;
            }

            const auto size = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(size_data->data());
            if (size > maximum_payload_size)
            {
                return std::nullopt;
            }

            auto data = receive_exact_until(socket, size, deadline);
            if (!data)
            {
                return std::nullopt;
            }
            size_data->append(*data);
            return decode_message(std::move(*size_data));
        }

        ProcessProtocol::ProcessError encode_error(const process_error error)
        {
            return static_cast<ProcessProtocol::ProcessError>(error);
        }

        process_error decode_error(const ProcessProtocol::ProcessError error)
        {
            return static_cast<process_error>(error);
        }

        ProcessProtocol::MemoryPermission encode_permission(const memory_permission permission)
        {
            return static_cast<ProcessProtocol::MemoryPermission>(permission);
        }

        memory_permission decode_permission(const ProcessProtocol::MemoryPermission permission)
        {
            return static_cast<memory_permission>(permission);
        }

        ProcessProtocol::ProcessMemoryResponseT encode_memory_result(const process_memory_result& result)
        {
            ProcessProtocol::ProcessMemoryResponseT response{};
            response.error = encode_error(result.error);
            response.address = result.address;
            response.size = result.size;
            response.permission = encode_permission(result.permission);
            return response;
        }

        process_memory_result decode_memory_result(const ProcessProtocol::ProcessMemoryResponseT& result)
        {
            return {.address = result.address,
                    .size = result.size,
                    .permission = decode_permission(result.permission),
                    .error = decode_error(result.error)};
        }

        ProcessProtocol::ProcessCreateRequestT encode_create_request(const process_create_request& request)
        {
            ProcessProtocol::ProcessCreateRequestT result{};
            result.application = request.application;
            result.argument0 = request.argument0;
            result.working_directory = request.working_directory;
            result.arguments = request.arguments;
            result.process_id = request.process_id;
            result.thread_id = request.thread_id;
            result.environment.reserve(request.environment.size());
            for (const auto& [name, value] : request.environment)
            {
                auto variable = std::make_unique<ProcessProtocol::EnvironmentVariableT>();
                variable->name = name;
                variable->value = value;
                result.environment.push_back(std::move(variable));
            }
            return result;
        }

        process_create_request decode_create_request(ProcessProtocol::ProcessCreateRequestT request)
        {
            process_create_request result{.application = std::move(request.application),
                                          .argument0 = std::move(request.argument0),
                                          .working_directory = std::move(request.working_directory),
                                          .arguments = std::move(request.arguments),
                                          .process_id = request.process_id,
                                          .thread_id = request.thread_id};
            result.environment.reserve(request.environment.size());
            for (auto& variable : request.environment)
            {
                result.environment.insert_or_assign(std::move(variable->name), std::move(variable->value));
            }
            return result;
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

    struct subprocess_process_manager::process_entry
    {
        reproc::process process{};
        network::tcp_client_socket control{};
        std::thread output_thread{};
        mutable std::mutex control_mutex{};
        mutable std::mutex state_mutex{};
        mutable std::optional<process_exit> exit{};
        std::optional<uint64_t> termination_code{};
        std::optional<int> host_exit_status{};
        bool host_failure{};
        std::atomic_bool kill_requested{};

        std::optional<ProcessProtocol::MessageT> receive_response(const ProcessProtocol::MessagePayload expected,
                                                                  const std::chrono::steady_clock::time_point deadline)
        {
            auto message = receive_message_until(this->control, deadline);
            if (!message)
            {
                return std::nullopt;
            }
            if (message->payload.type == ProcessProtocol::MessagePayload_ProcessExited)
            {
                const std::scoped_lock lock(this->state_mutex);
                this->exit = process_exit{.kind = process_exit_kind::exited, .code = message->payload.AsProcessExited()->exit_code};
            }
            return message->payload.type == expected ? std::move(message) : std::nullopt;
        }

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

    managed_process_connection::managed_process_connection(managed_process_connection&& other) noexcept
        : socket_(std::move(other.socket_)),
          request_(std::move(other.request_))
    {
    }

    managed_process_connection::~managed_process_connection()
    {
        this->disconnect();
    }

    const process_create_request& managed_process_connection::request() const
    {
        return this->request_;
    }

    bool managed_process_connection::wait_for_resume(managed_process_target& target)
    {
        ProcessProtocol::ProcessStartedT started{};
        started.native_environment = target.native_environment();
        started.compatibility_environment = target.compatibility_environment();
        started.native_parameters = target.native_parameters();
        started.compatibility_parameters = target.compatibility_parameters();
        {
            const std::scoped_lock lock(this->send_mutex_);
            if (!send_message(this->socket_, started))
            {
                return false;
            }
        }

        if (!this->handle_messages(target, true))
        {
            return false;
        }

        this->control_thread_ = std::thread([this, &target] { (void)this->handle_messages(target, false); });
        return true;
    }

    bool managed_process_connection::handle_messages(managed_process_target& target, const bool wait_for_resume)
    {
        while (const auto message = receive_message(this->socket_))
        {
            switch (message->payload.type)
            {
            case ProcessProtocol::MessagePayload_ResumeProcessRequest: {
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    if (!send_message(this->socket_, ProcessProtocol::ResumeProcessResponseT{}))
                    {
                        return false;
                    }
                }
                if (wait_for_resume)
                {
                    return true;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_AllocateMemoryRequest: {
                const auto& request = *message->payload.AsAllocateMemoryRequest();
                const auto result = target.allocate_memory(request.address, request.size, decode_permission(request.permission),
                                                           request.reserve, request.commit);
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    if (!send_message(this->socket_, encode_memory_result(result)))
                    {
                        return false;
                    }
                }
                break;
            }
            case ProcessProtocol::MessagePayload_FreeMemoryRequest: {
                const auto& request = *message->payload.AsFreeMemoryRequest();
                const auto error = target.free_memory(request.address, request.size, request.release);
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    ProcessProtocol::ProcessErrorResponseT response{};
                    response.error = encode_error(error);
                    if (!send_message(this->socket_, response))
                    {
                        return false;
                    }
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ProtectMemoryRequest: {
                const auto& request = *message->payload.AsProtectMemoryRequest();
                const auto result = target.protect_memory(request.address, request.size, decode_permission(request.permission));
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    if (!send_message(this->socket_, encode_memory_result(result)))
                    {
                        return false;
                    }
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ReadMemoryRequest: {
                const auto& request = *message->payload.AsReadMemoryRequest();
                auto result = target.read_memory(request.address, request.size);
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    ProcessProtocol::ReadMemoryResponseT response{};
                    response.error = encode_error(result.error);
                    response.data = std::move(result.data);
                    if (!send_message(this->socket_, std::move(response)))
                    {
                        return false;
                    }
                }
                break;
            }
            case ProcessProtocol::MessagePayload_WriteMemoryRequest: {
                const auto& request = *message->payload.AsWriteMemoryRequest();
                const auto result = target.write_memory(request.address, request.data);
                {
                    const std::scoped_lock lock(this->send_mutex_);
                    if (!send_message(this->socket_, encode_memory_result(result)))
                    {
                        return false;
                    }
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool managed_process_connection::notify_exit(const uint64_t exit_code)
    {
        ProcessProtocol::ProcessExitedT message{};
        message.exit_code = exit_code;
        bool sent{};
        {
            const std::scoped_lock lock(this->send_mutex_);
            sent = send_message(this->socket_, message);
        }
        this->disconnect();
        return sent;
    }

    void managed_process_connection::disconnect()
    {
        this->socket_.close();
        if (this->control_thread_.joinable())
        {
            this->control_thread_.join();
        }
    }

    managed_process_connection connect_managed_process(const uint16_t port, const std::string& token)
    {
        network::tcp_client_socket socket{AF_INET};
        ProcessProtocol::HelloT hello{};
        hello.token = token;
        if (!socket.connect(network::address{"127.0.0.1", port}) || !send_message(socket, std::move(hello)))
        {
            throw std::runtime_error("Connecting to the managed process parent failed");
        }

        auto message = receive_message(socket);
        if (!message || message->payload.type != ProcessProtocol::MessagePayload_ProcessCreateRequest)
        {
            throw std::runtime_error("Receiving the managed process request failed");
        }
        return {std::move(socket), decode_create_request(std::move(*message->payload.AsProcessCreateRequest()))};
    }

    subprocess_process_manager::subprocess_process_manager(std::filesystem::path executable, argument_factory arguments)
        : executable_(std::move(executable)),
          arguments_(std::move(arguments))
    {
        if (this->executable_.empty() || !this->arguments_)
        {
            throw std::invalid_argument("An executable and argument factory are required");
        }
    }

    subprocess_process_manager::~subprocess_process_manager()
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

    process_create_result subprocess_process_manager::create_process(process_create_request request)
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
        const auto start_error = entry->process.start(command, options);
        if (start_error)
        {
            return {.error = map_process_error(start_error)};
        }

        const auto [host_process_id, process_id_error] = entry->process.pid();
        if (process_id_error || host_process_id <= 0)
        {
            (void)entry->process.kill();
            (void)entry->process.wait(reproc::infinite);
            return {.error = process_error::internal_failure};
        }
        request.process_id = static_cast<uint32_t>(host_process_id) * 2;
        request.thread_id = request.process_id + 1;

        entry->output_thread = std::thread([process = entry.get()] {
            std::array<uint8_t, 4096> buffer{};
            for (;;)
            {
                if (process->kill_requested)
                {
                    const auto kill_error = process->process.kill();
                    if (!kill_error)
                    {
                        process->kill_requested = false;
                    }
                    else
                    {
                        const std::scoped_lock lock(process->state_mutex);
                        process->host_failure = true;
                    }
                }
                const auto [events, poll_error] =
                    process->process.poll(reproc::event::out | reproc::event::err | reproc::event::exit, reproc::milliseconds(50));
                if (poll_error && poll_error != std::errc::timed_out)
                {
                    const std::scoped_lock lock(process->state_mutex);
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
                    const std::scoped_lock lock(process->state_mutex);
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
            if (hello && hello->payload.type == ProcessProtocol::MessagePayload_Hello && hello->payload.AsHello()->token == token)
            {
                entry->control = std::move(candidate);
                break;
            }
        }

        if (!entry->control || !send_message(entry->control, encode_create_request(request)))
        {
            entry->kill_requested = true;
            return {.error = process_error::communication_failure};
        }

        const auto started = receive_message_until(entry->control, deadline);
        if (!started || started->payload.type != ProcessProtocol::MessagePayload_ProcessStarted)
        {
            entry->kill_requested = true;
            return {.error = process_error::internal_failure};
        }
        {
            const std::scoped_lock lock(this->mutex_);
            if (this->stopping_)
            {
                entry->kill_requested = true;
                return {.error = process_error::unavailable};
            }
            this->processes_.emplace(handle.value, entry);
        }
        const auto& process_started = *started->payload.AsProcessStarted();
        return {.process = handle,
                .process_id = request.process_id,
                .thread_id = request.thread_id,
                .native_environment = process_started.native_environment,
                .compatibility_environment = process_started.compatibility_environment,
                .native_parameters = process_started.native_parameters,
                .compatibility_parameters = process_started.compatibility_parameters};
    }

    process_error subprocess_process_manager::resume_process(const managed_process process)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, ProcessProtocol::ResumeProcessRequestT{}))
        {
            return process_error::communication_failure;
        }
        auto response = entry->receive_response(ProcessProtocol::MessagePayload_ResumeProcessResponse,
                                                std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ResumeProcessResponse)
        {
            return process_error::communication_failure;
        }
        return decode_error(response->payload.AsResumeProcessResponse()->error);
    }

    process_memory_result subprocess_process_manager::allocate_memory(const managed_process process, const uint64_t address,
                                                                      const uint64_t size, const memory_permission permission,
                                                                      const bool reserve, const bool commit)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        ProcessProtocol::AllocateMemoryRequestT request{};
        request.address = address;
        request.size = size;
        request.permission = encode_permission(permission);
        request.reserve = reserve;
        request.commit = commit;
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, request))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = entry->receive_response(ProcessProtocol::MessagePayload_ProcessMemoryResponse,
                                                      std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ProcessMemoryResponse)
        {
            return {.error = process_error::communication_failure};
        }
        return decode_memory_result(*response->payload.AsProcessMemoryResponse());
    }

    process_error subprocess_process_manager::free_memory(const managed_process process, const uint64_t address, const uint64_t size,
                                                          const bool release)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }
        ProcessProtocol::FreeMemoryRequestT request{};
        request.address = address;
        request.size = size;
        request.release = release;
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, request))
        {
            return process_error::communication_failure;
        }
        const auto response = entry->receive_response(ProcessProtocol::MessagePayload_ProcessErrorResponse,
                                                      std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ProcessErrorResponse)
        {
            return process_error::communication_failure;
        }
        return decode_error(response->payload.AsProcessErrorResponse()->error);
    }

    process_memory_result subprocess_process_manager::protect_memory(const managed_process process, const uint64_t address,
                                                                     const uint64_t size, const memory_permission permission)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        ProcessProtocol::ProtectMemoryRequestT request{};
        request.address = address;
        request.size = size;
        request.permission = encode_permission(permission);
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, request))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = entry->receive_response(ProcessProtocol::MessagePayload_ProcessMemoryResponse,
                                                      std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ProcessMemoryResponse)
        {
            return {.error = process_error::communication_failure};
        }
        return decode_memory_result(*response->payload.AsProcessMemoryResponse());
    }

    process_memory_read_result subprocess_process_manager::read_memory(const managed_process process, const uint64_t address,
                                                                       const uint64_t size) const
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }
        ProcessProtocol::ReadMemoryRequestT request{};
        request.address = address;
        request.size = size;
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, request))
        {
            return {.error = process_error::communication_failure};
        }
        auto response = entry->receive_response(ProcessProtocol::MessagePayload_ReadMemoryResponse,
                                                std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ReadMemoryResponse)
        {
            return {.error = process_error::communication_failure};
        }
        auto& result = *response->payload.AsReadMemoryResponse();
        return {.data = std::move(result.data), .error = decode_error(result.error)};
    }

    process_memory_result subprocess_process_manager::write_memory(const managed_process process, const uint64_t address,
                                                                   const std::span<const uint8_t> data)
    {
        const auto entry = this->find_process(process);
        if (!entry || data.size() > maximum_payload_size)
        {
            return {.error = entry ? process_error::resource_limit : process_error::invalid_process};
        }
        ProcessProtocol::WriteMemoryRequestT request{};
        request.address = address;
        request.data.assign(data.begin(), data.end());
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, std::move(request)))
        {
            return {.error = process_error::communication_failure};
        }
        const auto response = entry->receive_response(ProcessProtocol::MessagePayload_ProcessMemoryResponse,
                                                      std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != ProcessProtocol::MessagePayload_ProcessMemoryResponse)
        {
            return {.error = process_error::communication_failure};
        }
        return decode_memory_result(*response->payload.AsProcessMemoryResponse());
    }

    process_error subprocess_process_manager::terminate_process(const managed_process process, const uint64_t exit_code)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }

        {
            const std::scoped_lock lock(entry->state_mutex);
            if (entry->exit)
            {
                return process_error::none;
            }
            entry->termination_code = exit_code;
        }
        entry->kill_requested = true;
        return process_error::none;
    }

    process_exit_status_result subprocess_process_manager::exit_status(const managed_process process) const
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {.error = process_error::invalid_process};
        }

        bool host_exited{};
        {
            const std::scoped_lock lock(entry->state_mutex);
            if (entry->exit)
            {
                return {.status = entry->exit};
            }
            if (!entry->host_exit_status && !entry->host_failure)
            {
                return {};
            }
            host_exited = entry->host_exit_status.has_value();
        }

        if (host_exited)
        {
            const std::scoped_lock lock(entry->control_mutex);
            (void)entry->receive_response(ProcessProtocol::MessagePayload_ProcessExited,
                                          std::chrono::steady_clock::now() + exit_notification_timeout);
        }

        const std::scoped_lock lock(entry->state_mutex);
        if (entry->exit)
        {
            return {.status = entry->exit};
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

    std::shared_ptr<subprocess_process_manager::process_entry> subprocess_process_manager::find_process(const managed_process process) const
    {
        const std::scoped_lock lock(this->mutex_);
        const auto entry = this->processes_.find(process.value);
        return entry == this->processes_.end() ? nullptr : entry->second;
    }
}
