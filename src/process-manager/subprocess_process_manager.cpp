#include "subprocess_process_manager.hpp"
#include "process_protocol_utils.hpp"

#include <network/address.hpp>
#include <network/tcp_server_socket.hpp>
#include <utils/host_pipe.hpp>
#include <utils/finally.hpp>

#ifdef poll
#undef poll
#endif

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

// Something above pulls in excpt.h, which defines these as macros for the SEH keywords and so turns the identically
// named fields of process_debug_event into a compile error. Which includes bring it in differs by configuration, so
// the undef has to come after all of them.
#undef exception_code
#undef exception_info

namespace sogen
{
    namespace
    {
        using namespace process_protocol; // NOLINT(google-build-using-namespace)
        constexpr auto exit_notification_timeout = std::chrono::milliseconds(250);

        void encode_token(const process_token& token, uint64_t& identity, bool& has_privileges,
                          std::vector<ProcessProtocol::TokenPrivilege>& privileges)
        {
            identity = token.identity;
            has_privileges = token.privileges.has_value();
            privileges.clear();
            if (!token.privileges)
            {
                return;
            }

            privileges.reserve(token.privileges->size());
            for (const auto& privilege : *token.privileges)
            {
                privileges.emplace_back(privilege.luid, privilege.attributes);
            }
        }

        process_token decode_token(const uint64_t identity, const bool has_privileges,
                                   const std::vector<ProcessProtocol::TokenPrivilege>& privileges)
        {
            process_token token{.identity = identity};
            if (has_privileges)
            {
                auto& decoded = token.privileges.emplace();
                decoded.reserve(privileges.size());
                for (const auto& privilege : privileges)
                {
                    decoded.push_back({.luid = privilege.luid(), .attributes = privilege.attributes()});
                }
            }
            return token;
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

        std::unique_ptr<ProcessProtocol::SharedObjectT> encode_shared_object(const process_shared_object& object)
        {
            auto result = std::make_unique<ProcessProtocol::SharedObjectT>();
            result->kind = static_cast<ProcessProtocol::SharedObjectKind>(object.kind);
            result->guest_handle = object.guest_handle;
            result->memory = object.memory;
            result->size = object.size;
            result->protection = object.protection;
            result->attributes = object.attributes;
            result->event_type = object.event_type;
            result->access = object.access;
            result->inherit = object.inherit;
            result->protect_from_close = object.protect_from_close;
            return result;
        }

        process_shared_object decode_shared_object(const ProcessProtocol::SharedObjectT& object)
        {
            return {.kind = static_cast<process_shared_object_kind>(object.kind),
                    .guest_handle = object.guest_handle,
                    .memory = object.memory,
                    .size = object.size,
                    .protection = object.protection,
                    .attributes = object.attributes,
                    .event_type = object.event_type,
                    .access = object.access,
                    .inherit = object.inherit,
                    .protect_from_close = object.protect_from_close};
        }

        ProcessProtocol::ProcessCreateRequestT encode_create_request(const process_create_request& request,
                                                                     std::vector<utils::host_pipe_transfer>& transfers)
        {
            ProcessProtocol::ProcessCreateRequestT result{};
            result.application = request.application;
            result.argument0 = request.argument0;
            result.command_line = request.command_line;
            result.working_directory = request.working_directory;
            result.arguments = request.arguments;
            result.process_id = request.process_id;
            result.thread_id = request.thread_id;
            result.standard_input = request.standard_input;
            result.standard_output = request.standard_output;
            result.standard_error = request.standard_error;
            result.debugged = request.debugged;
            result.parent_process_id = request.parent_process_id;
            result.registry_state = request.registry_state;
            result.session_port = request.session.port;
            result.session_token = request.session.token;
            std::unordered_map<uint64_t, std::pair<uint16_t, std::string>> exported_files;
            for (const auto& pipe : request.inherited_pipes)
            {
                auto inherited = std::make_unique<ProcessProtocol::InheritedPipeT>();
                inherited->guest_handle = pipe.guest_handle;
                inherited->file_id = pipe.file_id ? pipe.file_id : pipe.guest_handle;
                auto [file, inserted] = exported_files.try_emplace(inherited->file_id);
                if (inserted)
                {
                    auto transfer = pipe.endpoint->export_endpoint();
                    file->second = {transfer.port(), transfer.token()};
                    transfers.push_back(std::move(transfer));
                }
                inherited->port = file->second.first;
                inherited->token = file->second.second;
                inherited->name = pipe.name;
                inherited->access = pipe.access;
                inherited->pipe_type = pipe.pipe_type;
                inherited->read_mode = pipe.read_mode;
                inherited->completion_mode = pipe.completion_mode;
                inherited->create_options = pipe.create_options;
                inherited->completion_notification_flags = pipe.completion_notification_flags;
                inherited->inherit = pipe.inherit;
                result.inherited_pipes.push_back(std::move(inherited));
            }
            for (const auto& object : request.inherited_objects)
            {
                result.inherited_objects.push_back(encode_shared_object(object));
            }
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
                                          .command_line = std::move(request.command_line),
                                          .working_directory = std::move(request.working_directory),
                                          .arguments = std::move(request.arguments),
                                          .process_id = request.process_id,
                                          .thread_id = request.thread_id};
            result.standard_input = request.standard_input;
            result.standard_output = request.standard_output;
            result.standard_error = request.standard_error;
            result.debugged = request.debugged;
            result.parent_process_id = request.parent_process_id;
            result.registry_state = std::move(request.registry_state);
            result.session = {.port = request.session_port, .token = std::move(request.session_token)};
            std::unordered_map<uint64_t, std::shared_ptr<utils::host_pipe>> imported_files;
            for (const auto& pipe : request.inherited_pipes)
            {
                const auto file_id = pipe->file_id ? pipe->file_id : pipe->guest_handle;
                auto [file, inserted] = imported_files.try_emplace(file_id);
                if (inserted)
                {
                    file->second = utils::host_pipe::connect(pipe->port, pipe->token);
                }
                result.inherited_pipes.push_back({.guest_handle = pipe->guest_handle,
                                                  .file_id = file_id,
                                                  .endpoint = file->second,
                                                  .name = pipe->name,
                                                  .access = pipe->access,
                                                  .pipe_type = pipe->pipe_type,
                                                  .read_mode = pipe->read_mode,
                                                  .completion_mode = pipe->completion_mode,
                                                  .create_options = pipe->create_options,
                                                  .completion_notification_flags = pipe->completion_notification_flags,
                                                  .inherit = pipe->inherit});
            }
            for (const auto& object : request.inherited_objects)
            {
                result.inherited_objects.push_back(decode_shared_object(*object));
            }
            result.environment.reserve(request.environment.size());
            for (auto& variable : request.environment)
            {
                result.environment.insert_or_assign(std::move(variable->name), std::move(variable->value));
            }
            return result;
        }

        std::unique_ptr<ProcessProtocol::DebugEventT> encode_debug_event(const process_debug_event& event)
        {
            auto result = std::make_unique<ProcessProtocol::DebugEventT>();
            result->kind = static_cast<ProcessProtocol::DebugEventKind>(event.kind);
            result->thread_id = event.thread_id;
            result->base_address = event.base_address;
            result->start_address = event.start_address;
            result->path = event.path;
            result->exit_status = event.exit_status;
            result->exception_code = event.exception_code;
            result->exception_flags = event.exception_flags;
            result->exception_address = event.exception_address;
            result->exception_parameters = event.exception_parameters;
            result->first_chance = event.first_chance;
            return result;
        }

        process_debug_event decode_debug_event(ProcessProtocol::DebugEventT& event)
        {
            return {.kind = static_cast<process_debug_event_kind>(event.kind),
                    .thread_id = event.thread_id,
                    .base_address = event.base_address,
                    .start_address = event.start_address,
                    .path = std::move(event.path),
                    .exit_status = event.exit_status,
                    .exception_code = event.exception_code,
                    .exception_flags = event.exception_flags,
                    .exception_address = event.exception_address,
                    .exception_parameters = std::move(event.exception_parameters),
                    .first_chance = event.first_chance};
        }

        ProcessProtocol::QueryMemoryResponseT encode_memory_region(const process_memory_region& region)
        {
            ProcessProtocol::QueryMemoryResponseT result{};
            result.error = encode_error(region.error);
            result.base_address = region.base_address;
            result.allocation_base = region.allocation_base;
            result.allocation_protect = region.allocation_protect;
            result.region_size = region.region_size;
            result.state = region.state;
            result.protect = region.protect;
            result.type = region.type;
            result.mapped_file = region.mapped_file;
            return result;
        }

    }

    struct subprocess_process_manager::child_process
    {
        process_session* session{};
        std::shared_ptr<session_process> status{};
        std::unique_ptr<host_process> host{};
        network::tcp_client_socket control{};
        std::vector<utils::host_pipe_transfer> pipe_transfers{};
        std::thread output_thread{};
        std::thread exit_thread{};
        mutable std::mutex control_mutex{};
        mutable std::mutex state_mutex{};
        std::optional<uint64_t> termination_code{};
        std::condition_variable host_finished{};
        std::optional<int> host_exit_status{};
        bool host_exited{};
        std::atomic_bool resumed{};

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
                const auto& exited = *message->payload.AsProcessExited();
                this->session->mark_exit(this->status->identity().process_id,
                                         {.kind = process_exit_kind::exited, .code = exited.exit_code, .cycles = exited.cycles});
            }
            return message->payload.type == expected ? std::move(message) : std::nullopt;
        }

        void monitor_exit()
        {
            std::unique_lock state(this->state_mutex);
            while (!this->host_exited)
            {
                state.unlock();
                if (std::unique_lock control_lock(this->control_mutex, std::try_to_lock);
                    control_lock.owns_lock() && this->control.is_ready(true))
                {
                    (void)this->receive_response(ProcessProtocol::MessagePayload_ProcessExited,
                                                 std::chrono::steady_clock::now() + exit_notification_timeout);
                }
                state.lock();
                this->host_finished.wait_for(state, std::chrono::milliseconds(10), [this] { return this->host_exited; });
            }
            process_exit exit{.kind = process_exit_kind::runtime_failure,
                              .code = this->host_exit_status ? static_cast<uint32_t>(*this->host_exit_status) : 1u};
            if (this->termination_code)
            {
                exit = {.kind = process_exit_kind::terminated, .code = *this->termination_code};
            }
            state.unlock();
            {
                const std::scoped_lock control_lock(this->control_mutex);
                (void)this->receive_response(ProcessProtocol::MessagePayload_ProcessExited,
                                             std::chrono::steady_clock::now() + exit_notification_timeout);
            }
            this->session->mark_exit(this->status->identity().process_id, exit);
        }

        void kill() const
        {
            if (this->host)
            {
                this->host->kill();
            }
        }

        ~child_process()
        {
            this->kill();
            if (this->output_thread.joinable())
            {
                this->output_thread.join();
            }
            if (this->exit_thread.joinable())
            {
                this->exit_thread.join();
            }
            this->control.close();
        }
    };

    struct subprocess_process_manager::control_response
    {
        std::optional<ProcessProtocol::MessageT> message{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->message.has_value();
        }

        ProcessProtocol::MessageT* operator->()
        {
            return &*this->message;
        }

        const ProcessProtocol::MessageT* operator->() const
        {
            return &*this->message;
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

    std::vector<process_inherited_pipe> managed_process_connection::take_inherited_pipes()
    {
        return std::exchange(this->request_.inherited_pipes, {});
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
            case ProcessProtocol::MessagePayload_DebugEventRequest: {
                const auto result = target.next_debug_event();
                ProcessProtocol::DebugEventResponseT response{};
                response.error = encode_error(result.error);
                if (result.event)
                {
                    response.event = encode_debug_event(*result.event);
                }
                if (!this->send(std::move(response)))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_DebugContinueRequest: {
                const auto& request = *message->payload.AsDebugContinueRequest();
                ProcessProtocol::ProcessErrorResponseT response{};
                response.error = encode_error(target.continue_debug_event(request.thread_id, request.status));
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_DebugDetachRequest: {
                ProcessProtocol::ProcessErrorResponseT response{};
                response.error = encode_error(target.detach_debugger());
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ThreadContextRequest: {
                const auto& request = *message->payload.AsThreadContextRequest();
                auto result = target.get_thread_context(request.thread_id, request.wow64);
                ProcessProtocol::ThreadContextResponseT response{};
                response.error = encode_error(result.error);
                response.context = std::move(result.context);
                if (!this->send(std::move(response)))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_SetThreadContextRequest: {
                const auto& request = *message->payload.AsSetThreadContextRequest();
                ProcessProtocol::ProcessErrorResponseT response{};
                response.error = encode_error(target.set_thread_context(request.thread_id, request.context, request.wow64));
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ThreadInformationRequest: {
                const auto result = target.thread_information(message->payload.AsThreadInformationRequest()->thread_id);
                ProcessProtocol::ThreadInformationResponseT response{};
                response.error = encode_error(result.error);
                response.teb = result.teb;
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_SuspendThreadRequest: {
                const auto& request = *message->payload.AsSuspendThreadRequest();
                const auto result = target.suspend_thread(request.thread_id, request.resume);
                ProcessProtocol::SuspendThreadResponseT response{};
                response.error = encode_error(result.error);
                response.previous_count = result.previous_count;
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_InstallObjectRequest: {
                const auto result = target.install_object(decode_shared_object(*message->payload.AsInstallObjectRequest()->object));
                ProcessProtocol::InstallObjectResponseT response{};
                response.error = encode_error(result.error);
                response.handle = result.handle;
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ExportObjectRequest: {
                const auto result = target.export_object(message->payload.AsExportObjectRequest()->handle);
                ProcessProtocol::ExportObjectResponseT response{};
                response.error = encode_error(result.error);
                if (result.error == process_error::none)
                {
                    response.object = encode_shared_object(result.object);
                }
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_CloseHandleRequest: {
                ProcessProtocol::ProcessErrorResponseT response{};
                response.error = encode_error(target.close_handle(message->payload.AsCloseHandleRequest()->handle));
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_SetThreadTokenRequest: {
                const auto& request = *message->payload.AsSetThreadTokenRequest();
                ProcessProtocol::ProcessErrorResponseT response{};
                response.error = encode_error(
                    target.set_thread_token(request.thread_id, decode_token(request.token, request.has_privileges, request.privileges)));
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_ThreadTokenRequest: {
                const auto result = target.get_thread_token(message->payload.AsThreadTokenRequest()->thread_id);
                ProcessProtocol::ThreadTokenResponseT response{};
                response.error = encode_error(result.error);
                encode_token(result.token, response.token, response.has_privileges, response.privileges);
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_CycleTimeRequest: {
                const auto result = target.cycle_time();
                ProcessProtocol::CycleTimeResponseT response{};
                response.error = encode_error(result.error);
                response.cycles = result.cycles;
                if (!this->send(response))
                {
                    return false;
                }
                break;
            }
            case ProcessProtocol::MessagePayload_QueryMemoryRequest: {
                const auto region = target.query_memory(message->payload.AsQueryMemoryRequest()->address);
                if (!this->send(encode_memory_region(region)))
                {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    template <typename T>
    bool managed_process_connection::send(T payload)
    {
        const std::scoped_lock lock(this->send_mutex_);
        return send_message(this->socket_, std::move(payload));
    }

    bool managed_process_connection::notify_exit(const uint64_t exit_code, const uint64_t cycles)
    {
        ProcessProtocol::ProcessExitedT message{};
        message.exit_code = exit_code;
        message.cycles = cycles;
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

    subprocess_process_manager::subprocess_process_manager(std::filesystem::path executable, argument_factory arguments,
                                                           std::unique_ptr<host_process_launcher> launcher,
                                                           process_session_endpoint session, std::chrono::milliseconds session_timeout,
                                                           process_session::clock_source session_clock)
        : executable_(std::move(executable)),
          arguments_(std::move(arguments)),
          launcher_(std::move(launcher)),
          session_(std::move(session), std::move(session_clock), session_timeout)
    {
        if (this->executable_.empty() || !this->arguments_ || !this->launcher_)
        {
            throw std::invalid_argument("An executable and argument factory are required");
        }
    }

    subprocess_process_manager::~subprocess_process_manager()
    {
        if (this->current_process_id_ && !this->current_process_exited_)
        {
            this->session_.mark_exit(this->current_process_id_, {.kind = process_exit_kind::runtime_failure, .code = 1});
        }
        std::vector<std::shared_ptr<child_process>> processes{};
        {
            const std::scoped_lock lock(this->mutex_);
            this->stopping_ = true;
            processes.reserve(this->processes_.size());
            for (const auto& [handle, process] : this->processes_)
            {
                (void)handle;
                if (const auto* child = std::get_if<std::shared_ptr<child_process>>(&process))
                {
                    processes.push_back(*child);
                }
            }
        }

        for (const auto& process : processes)
        {
            process->kill();
        }
    }

    process_session_endpoint subprocess_process_manager::session_endpoint() const
    {
        return this->session_.endpoint();
    }

    void subprocess_process_manager::initialize_current_process(const process_identity identity, const uint32_t initial_thread_id)
    {
        if (identity.process_id == 0)
        {
            throw std::invalid_argument("The current process must have a nonzero guest PID");
        }
        if (this->current_process_id_ == identity.process_id)
        {
            return;
        }
        if (this->session_.owns_registry())
        {
            const auto registered = this->session_.register_process(identity, initial_thread_id);
            if (!registered)
            {
                throw std::runtime_error("Registering the guest process failed");
            }
        }
        this->current_process_id_ = identity.process_id;
        this->current_process_exited_ = false;
    }

    std::optional<guest_id_result> subprocess_process_manager::allocate_thread_id(const uint32_t requested_id)
    {
        return this->session_.allocate_thread_id(this->current_process_id_, requested_id);
    }

    void subprocess_process_manager::notify_current_process_exit(const uint64_t exit_code)
    {
        if (this->session_.mark_exit(this->current_process_id_, {.kind = process_exit_kind::exited, .code = exit_code}) ==
            process_error::none)
        {
            this->current_process_exited_ = true;
        }
    }

    process_open_result subprocess_process_manager::open_process(const uint32_t process_id)
    {
        const std::scoped_lock publication(this->publication_mutex_);
        const auto result = this->session_.lookup(process_id);
        if (!result)
        {
            return {.error = result.error};
        }
        const std::scoped_lock lock(this->mutex_);
        for (const auto& [handle, reference] : this->processes_)
        {
            if (const auto* child = std::get_if<std::shared_ptr<child_process>>(&reference);
                child && (*child)->status->identity().process_id == result.process->identity().process_id)
            {
                return {.process = {.handle = managed_process{handle}, .identity = result.process->identity()}};
            }
            if (const auto* opened = std::get_if<std::shared_ptr<session_process>>(&reference);
                opened && (*opened)->identity().process_id == result.process->identity().process_id)
            {
                return {.process = {.handle = managed_process{handle}, .identity = result.process->identity()}};
            }
        }
        if (this->stopping_ || this->next_process_ == 0)
        {
            return {.error = process_error::unavailable};
        }
        const managed_process handle{this->next_process_++};
        this->processes_.emplace(handle.value, result.process);
        return {.process = {.handle = handle, .identity = result.process->identity()}};
    }

    process_create_result subprocess_process_manager::create_process(process_create_request request)
    {
        request.session = this->session_.endpoint();
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

        auto reservation = this->session_.reserve_process();
        if (!reservation)
        {
            return {.error = reservation.error};
        }
        request.process_id = reservation.process_id;
        request.thread_id = reservation.thread_id;
        auto discard_reservation = utils::finally([&] { (void)this->session_.discard_process(request.process_id); });
        const auto entry = std::make_shared<child_process>();
        auto launched = this->launcher_->launch(command);
        if (!launched.process)
        {
            return {.error = launched.error};
        }
        entry->host = std::move(launched.process);
        entry->session = &this->session_;
        entry->output_thread = std::thread([process = entry.get()] {
            const auto status = process->host->run();
            {
                const std::scoped_lock lock(process->state_mutex);
                process->host_exit_status = status;
                process->host_exited = true;
            }
            process->host_finished.notify_one();
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

        bool sent{};
        try
        {
            sent = entry->control && send_message(entry->control, encode_create_request(request, entry->pipe_transfers));
        }
        catch (const std::exception&)
        {
            entry->kill();
            return {.error = process_error::communication_failure};
        }
        if (!sent)
        {
            entry->kill();
            return {.error = process_error::communication_failure};
        }

        const auto started = receive_message_until(entry->control, deadline);
        if (!started || started->payload.type != ProcessProtocol::MessagePayload_ProcessStarted)
        {
            entry->kill();
            return {.error = process_error::internal_failure};
        }
        const auto& process_started = *started->payload.AsProcessStarted();
        const process_identity identity{.process_id = request.process_id,
                                        .parent_process_id = request.parent_process_id,
                                        .native_environment = process_started.native_environment,
                                        .compatibility_environment = process_started.compatibility_environment};
        const std::scoped_lock publication(this->publication_mutex_);
        const auto registration = this->session_.publish_process(identity);
        if (!registration)
        {
            entry->kill();
            return {.error = registration.error};
        }
        {
            const std::scoped_lock lock(this->mutex_);
            if (this->stopping_)
            {
                entry->kill();
                return {.error = process_error::unavailable};
            }
            entry->status = registration.process;
            entry->exit_thread = std::thread([process = entry.get()] { process->monitor_exit(); });
            this->processes_.emplace(handle.value, entry);
            discard_reservation.cancel();
        }
        return {.process = {.handle = handle, .identity = identity},
                .thread_id = request.thread_id,
                .native_parameters = process_started.native_parameters,
                .compatibility_parameters = process_started.compatibility_parameters};
    }

    process_error subprocess_process_manager::resume_process(const managed_process process)
    {
        const auto [entry, lookup_error] = this->find_child(process);
        if (!entry)
        {
            return lookup_error;
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

        const auto error = decode_error(response->payload.AsResumeProcessResponse()->error);
        if (error == process_error::none)
        {
            entry->resumed = true;
        }
        return error;
    }

    void subprocess_process_manager::kill_processes()
    {
        const std::scoped_lock lock(this->mutex_);
        for (const auto& [handle, process] : this->processes_)
        {
            (void)handle;
            if (const auto* child = std::get_if<std::shared_ptr<child_process>>(&process))
            {
                (*child)->kill();
            }
        }
    }

    // A child that was never resumed cannot run without this process, so only resumed children are waited for; the
    // destructor still ends the others.
    void subprocess_process_manager::wait_for_processes()
    {
        std::vector<std::shared_ptr<child_process>> processes{};
        {
            const std::scoped_lock lock(this->mutex_);
            this->stopping_ = true;
            for (const auto& [handle, process] : this->processes_)
            {
                (void)handle;
                if (const auto* child = std::get_if<std::shared_ptr<child_process>>(&process); child && (*child)->resumed)
                {
                    processes.push_back(*child);
                }
            }
        }

        for (const auto& process : processes)
        {
            if (process->output_thread.joinable())
            {
                process->output_thread.join();
            }
        }
    }

    process_memory_result subprocess_process_manager::allocate_memory(const managed_process process, const uint64_t address,
                                                                      const uint64_t size, const memory_permission permission,
                                                                      const bool reserve, const bool commit)
    {
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return {.error = error};
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
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return error;
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
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return {.error = error};
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
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return {.error = error};
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
        const auto [entry, error] = this->find_child(process);
        if (!entry || data.size() > maximum_payload_size)
        {
            return {.error = entry ? process_error::resource_limit : error};
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

    template <typename Request>
    subprocess_process_manager::control_response subprocess_process_manager::exchange(const managed_process process, Request request,
                                                                                      const uint8_t response_type) const
    {
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return {.error = error};
        }
        const auto expected = static_cast<ProcessProtocol::MessagePayload>(response_type);
        const std::scoped_lock lock(entry->control_mutex);
        if (!send_message(entry->control, std::move(request)))
        {
            return {.error = process_error::communication_failure};
        }
        auto response = entry->receive_response(expected, std::chrono::steady_clock::now() + response_timeout);
        if (!response || response->payload.type != expected)
        {
            return {.error = process_error::communication_failure};
        }
        return {.message = std::move(response)};
    }

    process_debug_event_result subprocess_process_manager::next_debug_event(const managed_process process)
    {
        auto response = this->exchange(process, ProcessProtocol::DebugEventRequestT{}, ProcessProtocol::MessagePayload_DebugEventResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        auto& result = *response->payload.AsDebugEventResponse();
        process_debug_event_result decoded{.error = decode_error(result.error)};
        if (result.event)
        {
            decoded.event = decode_debug_event(*result.event);
        }
        return decoded;
    }

    process_error subprocess_process_manager::continue_debug_event(const managed_process process, const uint32_t thread_id,
                                                                   const uint32_t status)
    {
        ProcessProtocol::DebugContinueRequestT request{};
        request.thread_id = thread_id;
        request.status = status;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ProcessErrorResponse);
        return response ? decode_error(response->payload.AsProcessErrorResponse()->error) : response.error;
    }

    process_error subprocess_process_manager::detach_debugger(const managed_process process)
    {
        const auto response =
            this->exchange(process, ProcessProtocol::DebugDetachRequestT{}, ProcessProtocol::MessagePayload_ProcessErrorResponse);
        return response ? decode_error(response->payload.AsProcessErrorResponse()->error) : response.error;
    }

    process_thread_context_result subprocess_process_manager::get_thread_context(const managed_process process, const uint32_t thread_id,
                                                                                 const bool wow64)
    {
        ProcessProtocol::ThreadContextRequestT request{};
        request.thread_id = thread_id;
        request.wow64 = wow64;
        auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ThreadContextResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        auto& result = *response->payload.AsThreadContextResponse();
        return {.context = std::move(result.context), .error = decode_error(result.error)};
    }

    process_error subprocess_process_manager::set_thread_context(const managed_process process, const uint32_t thread_id,
                                                                 const std::span<const uint8_t> context, const bool wow64)
    {
        ProcessProtocol::SetThreadContextRequestT request{};
        request.thread_id = thread_id;
        request.wow64 = wow64;
        request.context.assign(context.begin(), context.end());
        const auto response = this->exchange(process, std::move(request), ProcessProtocol::MessagePayload_ProcessErrorResponse);
        return response ? decode_error(response->payload.AsProcessErrorResponse()->error) : response.error;
    }

    process_thread_information subprocess_process_manager::thread_information(const managed_process process, const uint32_t thread_id)
    {
        ProcessProtocol::ThreadInformationRequestT request{};
        request.thread_id = thread_id;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ThreadInformationResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsThreadInformationResponse();
        return {.teb = result.teb, .error = decode_error(result.error)};
    }

    process_suspend_result subprocess_process_manager::suspend_thread(const managed_process process, const uint32_t thread_id,
                                                                      const bool resume)
    {
        ProcessProtocol::SuspendThreadRequestT request{};
        request.thread_id = thread_id;
        request.resume = resume;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_SuspendThreadResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsSuspendThreadResponse();
        return {.previous_count = result.previous_count, .error = decode_error(result.error)};
    }

    process_object_install_result subprocess_process_manager::install_object(const managed_process process,
                                                                             const process_shared_object& object)
    {
        ProcessProtocol::InstallObjectRequestT request{};
        request.object = encode_shared_object(object);
        const auto response = this->exchange(process, std::move(request), ProcessProtocol::MessagePayload_InstallObjectResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsInstallObjectResponse();
        return {.handle = result.handle, .error = decode_error(result.error)};
    }

    process_object_export_result subprocess_process_manager::export_object(const managed_process process, const uint64_t handle)
    {
        ProcessProtocol::ExportObjectRequestT request{};
        request.handle = handle;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ExportObjectResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsExportObjectResponse();
        if (result.error != ProcessProtocol::ProcessError_None || !result.object)
        {
            return {.error =
                        result.error != ProcessProtocol::ProcessError_None ? decode_error(result.error) : process_error::internal_failure};
        }
        return {.object = decode_shared_object(*result.object)};
    }

    process_error subprocess_process_manager::close_handle(const managed_process process, const uint64_t handle)
    {
        ProcessProtocol::CloseHandleRequestT request{};
        request.handle = handle;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ProcessErrorResponse);
        if (!response)
        {
            return response.error;
        }
        return decode_error(response->payload.AsProcessErrorResponse()->error);
    }

    process_error subprocess_process_manager::set_thread_token(const managed_process process, const uint32_t thread_id,
                                                               const process_token& token)
    {
        ProcessProtocol::SetThreadTokenRequestT request{};
        request.thread_id = thread_id;
        encode_token(token, request.token, request.has_privileges, request.privileges);
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ProcessErrorResponse);
        if (!response)
        {
            return response.error;
        }
        return decode_error(response->payload.AsProcessErrorResponse()->error);
    }

    process_thread_token_result subprocess_process_manager::get_thread_token(const managed_process process, const uint32_t thread_id)
    {
        ProcessProtocol::ThreadTokenRequestT request{};
        request.thread_id = thread_id;
        const auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_ThreadTokenResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsThreadTokenResponse();
        return {.token = decode_token(result.token, result.has_privileges, result.privileges), .error = decode_error(result.error)};
    }

    process_cycle_time_result subprocess_process_manager::cycle_time(const managed_process process) const
    {
        const auto exit = this->exit_status(process);
        if (exit.error != process_error::none)
        {
            return {.error = exit.error};
        }
        if (exit.status)
        {
            return {.cycles = exit.status->cycles};
        }

        const auto response =
            this->exchange(process, ProcessProtocol::CycleTimeRequestT{}, ProcessProtocol::MessagePayload_CycleTimeResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        const auto& result = *response->payload.AsCycleTimeResponse();
        return {.cycles = result.cycles, .error = decode_error(result.error)};
    }

    process_memory_region subprocess_process_manager::query_memory(const managed_process process, const uint64_t address) const
    {
        ProcessProtocol::QueryMemoryRequestT request{};
        request.address = address;
        auto response = this->exchange(process, request, ProcessProtocol::MessagePayload_QueryMemoryResponse);
        if (!response)
        {
            return {.error = response.error};
        }
        auto& result = *response->payload.AsQueryMemoryResponse();
        return {.base_address = result.base_address,
                .allocation_base = result.allocation_base,
                .allocation_protect = result.allocation_protect,
                .region_size = result.region_size,
                .state = result.state,
                .protect = result.protect,
                .type = result.type,
                .mapped_file = std::move(result.mapped_file),
                .error = decode_error(result.error)};
    }

    process_error subprocess_process_manager::terminate_process(const managed_process process, const uint64_t exit_code)
    {
        const auto [entry, error] = this->find_child(process);
        if (!entry)
        {
            return error;
        }

        {
            const std::scoped_lock lock(entry->state_mutex);
            if (entry->status->exit_status().status)
            {
                return process_error::none;
            }
            entry->termination_code = exit_code;
        }
        entry->kill();
        return process_error::none;
    }

    process_exit_status_result subprocess_process_manager::poll_exit_status(const managed_process process) const
    {
        const auto reference = this->find_process(process);
        if (!reference)
        {
            return {.error = process_error::invalid_process};
        }
        if (const auto* opened = std::get_if<std::shared_ptr<session_process>>(&*reference))
        {
            return this->session_.poll_exit_status(*opened);
        }
        return this->session_.poll_exit_status(std::get<std::shared_ptr<child_process>>(*reference)->status);
    }

    process_exit_status_result subprocess_process_manager::exit_status(const managed_process process) const
    {
        const auto reference = this->find_process(process);
        if (!reference)
        {
            return {.error = process_error::invalid_process};
        }
        if (const auto* opened = std::get_if<std::shared_ptr<session_process>>(&*reference))
        {
            return this->session_.exit_status(*opened);
        }
        const auto& entry = std::get<std::shared_ptr<child_process>>(*reference);
        return this->session_.exit_status(entry->status);
    }

    std::optional<subprocess_process_manager::process_reference> subprocess_process_manager::find_process(
        const managed_process process) const
    {
        const std::scoped_lock lock(this->mutex_);
        const auto entry = this->processes_.find(process.value);
        return entry == this->processes_.end() ? std::nullopt : std::optional{entry->second};
    }

    subprocess_process_manager::child_process_result subprocess_process_manager::find_child(const managed_process process) const
    {
        const auto reference = this->find_process(process);
        if (!reference)
        {
            return {.error = process_error::invalid_process};
        }
        if (const auto* child = std::get_if<std::shared_ptr<child_process>>(&*reference))
        {
            return {.process = *child};
        }
        return {.error = process_error::unavailable};
    }
}
