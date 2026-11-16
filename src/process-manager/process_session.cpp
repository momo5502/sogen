#include "process_session.hpp"
#include "process_protocol_utils.hpp"
#include "process_session_registry.hpp"
#include <network/tcp_server_socket.hpp>
#include <condition_variable>
#include <algorithm>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <stdexcept>

namespace sogen
{
    using namespace process_protocol; // NOLINT(google-build-using-namespace)

    namespace
    {
        process_error decode_error(const ProcessProtocol::ProcessError error)
        {
            return static_cast<process_error>(error);
        }
    }

    const process_identity& session_process::identity() const
    {
        return this->identity_;
    }

    process_exit_status_result session_process::exit_status() const
    {
        const std::scoped_lock lock(this->mutex_);
        return this->status_;
    }

    void session_process::update_status(process_exit_status_result status)
    {
        const std::scoped_lock lock(this->mutex_);
        if (this->status_.status && status.status && this->status_.status->kind == status.status->kind &&
            this->status_.status->code == status.status->code)
        {
            this->status_.status->cycles = std::max(this->status_.status->cycles, status.status->cycles);
        }
        if (!this->status_.status)
        {
            this->status_ = status;
        }
    }

    struct process_session::implementation
    {
        clock_source now;
        std::chrono::milliseconds request_timeout;
        std::unordered_set<uint32_t> pending_discards{};
        std::unordered_map<uint32_t, process_exit> pending_exits{};
        std::mutex exit_mutex{};
        std::unique_ptr<process_session_registry> registry{};
        process_session_endpoint endpoint{};
        network::tcp_server_socket server{};
        std::atomic_bool stopping{};
        std::thread worker{};
        std::thread maintenance_worker{};
        std::mutex mutex{};
        std::condition_variable work_requested{};
        std::unordered_map<uint32_t, std::weak_ptr<session_process>> opened_processes{};
        std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> watched_processes{};

        explicit implementation(process_session_endpoint remote, clock_source clock, std::chrono::milliseconds timeout)
            : now(std::move(clock)),
              request_timeout(timeout),
              endpoint(std::move(remote))
        {
            if (this->endpoint.port)
            {
                this->maintenance_worker = std::thread([this] { this->maintain_session(); });
                return;
            }
            this->registry = std::make_unique<process_session_registry>(this->now);
            this->server = network::tcp_server_socket{AF_INET};
            if (!this->server.bind(network::address{"127.0.0.1", 0}))
            {
                throw std::runtime_error("Binding the process session failed");
            }
            this->server.listen();
            this->endpoint = {.port = this->server.get_port(), .token = make_token()};
            this->worker = std::thread([this] {
                while (!this->stopping)
                {
                    if (!this->server.sleep(std::chrono::milliseconds(10), true))
                    {
                        continue;
                    }
                    auto client = this->server.accept();
                    if (!client || !client.set_blocking(false))
                    {
                        continue;
                    }
                    const auto deadline = std::chrono::steady_clock::now() + response_timeout;
                    const auto message = receive_message_until(client, deadline, &this->stopping);
                    if (!message || message->payload.type != ProcessProtocol::MessagePayload_SessionRequest)
                    {
                        continue;
                    }
                    const auto& request = *message->payload.AsSessionRequest();
                    if (request.token != this->endpoint.token)
                    {
                        continue;
                    }
                    (void)send_message(client, this->handle(request), deadline, &this->stopping);
                }
            });
        }

        ~implementation()
        {
            {
                const std::scoped_lock lock(this->mutex);
                this->stopping = true;
            }
            this->work_requested.notify_all();
            if (this->maintenance_worker.joinable())
            {
                this->maintenance_worker.join();
            }
            if (this->worker.joinable())
            {
                this->worker.join();
            }
        }

        static process_exit_status_result decode_status(const ProcessProtocol::SessionProcessT& record)
        {
            if (!record.exited)
            {
                return {};
            }
            return {.status = process_exit{
                        .kind = static_cast<process_exit_kind>(record.exit_kind), .code = record.exit_code, .cycles = record.cycles}};
        }

        void prune_opened_processes()
        {
            for (auto entry = this->opened_processes.begin(); entry != this->opened_processes.end();)
            {
                if (entry->second.expired())
                {
                    this->watched_processes.erase(entry->first);
                    entry = this->opened_processes.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
        }

        void update_cached_status(const ProcessProtocol::SessionResponseT& response)
        {
            if (!response.process)
            {
                return;
            }
            const std::scoped_lock lock(this->mutex);
            const auto cached = this->opened_processes.find(response.process->process_id);
            if (cached != this->opened_processes.end())
            {
                if (const auto process = cached->second.lock())
                {
                    process->update_status(decode_status(*response.process));
                }
            }
        }

        ProcessProtocol::SessionResponseT handle(const ProcessProtocol::SessionRequestT& request)
        {
            auto response = this->registry->handle(request);
            this->update_cached_status(response);
            return response;
        }

        std::shared_ptr<session_process> track(const ProcessProtocol::SessionProcessT& record)
        {
            const std::scoped_lock lock(this->mutex);
            const auto cached = this->opened_processes.find(record.process_id);
            auto process = cached == this->opened_processes.end() ? nullptr : cached->second.lock();
            if (!process)
            {
                this->prune_opened_processes();
                process = std::make_shared<session_process>();
                process->identity_ = {.process_id = record.process_id,
                                      .parent_process_id = record.parent_process_id,
                                      .native_environment = record.native_environment,
                                      .compatibility_environment = record.compatibility_environment};
                this->opened_processes[record.process_id] = process;
            }
            auto status = decode_status(record);
            if (this->registry)
            {
                ProcessProtocol::SessionRequestT request{};
                request.process_id = record.process_id;
                const auto current = this->registry->handle(request);
                if (current.process)
                {
                    status = decode_status(*current.process);
                }
            }
            process->update_status(status);
            return process;
        }

        ProcessProtocol::SessionResponseT exchange(ProcessProtocol::SessionRequestT request)
        {
            if (this->worker.joinable())
            {
                return this->handle(request);
            }
            request.token = this->endpoint.token;
            network::tcp_client_socket client{AF_INET};
            const auto deadline = this->request_timeout == std::chrono::milliseconds::max()
                                      ? std::chrono::steady_clock::time_point::max()
                                      : std::chrono::steady_clock::now() + this->request_timeout;
            if (!client.connect(network::address{"127.0.0.1", this->endpoint.port}, deadline, &this->stopping) ||
                !send_message(client, std::move(request), deadline, &this->stopping))
            {
                ProcessProtocol::SessionResponseT response{};
                response.error = ProcessProtocol::ProcessError_CommunicationFailure;
                return response;
            }
            auto message = receive_message_until(client, deadline, &this->stopping);
            if (!message || message->payload.type != ProcessProtocol::MessagePayload_SessionResponse)
            {
                ProcessProtocol::SessionResponseT response{};
                response.error = ProcessProtocol::ProcessError_CommunicationFailure;
                return response;
            }
            auto response = std::move(*message->payload.AsSessionResponse());
            this->update_cached_status(response);
            return response;
        }

        process_session_lookup_result lookup(ProcessProtocol::SessionRequestT request)
        {
            const auto response = this->exchange(std::move(request));
            if (response.error != ProcessProtocol::ProcessError_None)
            {
                return {.error = decode_error(response.error)};
            }
            if (!response.process)
            {
                return {.error = process_error::invalid_process};
            }
            return {.process = this->track(*response.process)};
        }

        process_error report_exit(const uint32_t process_id, process_exit exit)
        {
            const std::scoped_lock reporting(this->exit_mutex);
            {
                const std::scoped_lock lock(this->mutex);
                if (const auto pending = this->pending_exits.find(process_id); pending != this->pending_exits.end())
                {
                    if (pending->second.kind == exit.kind && pending->second.code == exit.code)
                    {
                        pending->second.cycles = std::max(pending->second.cycles, exit.cycles);
                    }
                    exit = pending->second;
                }
            }
            ProcessProtocol::SessionRequestT request{};
            request.operation = ProcessProtocol::SessionOperation_Exit;
            request.process_id = process_id;
            request.update = std::make_unique<ProcessProtocol::SessionProcessT>();
            request.update->process_id = process_id;
            request.update->exited = true;
            request.update->exit_code = exit.code;
            request.update->exit_kind = static_cast<uint8_t>(exit.kind);
            request.update->cycles = exit.cycles;
            const auto error = decode_error(this->exchange(std::move(request)).error);
            {
                const std::scoped_lock lock(this->mutex);
                if (error == process_error::communication_failure)
                {
                    this->pending_exits.insert_or_assign(process_id, exit);
                }
                else
                {
                    this->pending_exits.erase(process_id);
                }
            }
            this->work_requested.notify_one();
            return error;
        }

        void maintain_session()
        {
            auto interval = std::chrono::milliseconds(25);
            std::unique_lock lock(this->mutex);
            while (!this->stopping)
            {
                this->work_requested.wait(lock, [this] {
                    return this->stopping || !this->watched_processes.empty() || !this->pending_discards.empty() ||
                           !this->pending_exits.empty();
                });
                if (this->stopping)
                {
                    return;
                }
                ProcessProtocol::SessionRequestT request{};
                request.operation = ProcessProtocol::SessionOperation_Refresh;
                for (auto entry = this->watched_processes.begin(); entry != this->watched_processes.end();)
                {
                    const auto cached = this->opened_processes.find(entry->first);
                    const auto process = cached == this->opened_processes.end() ? nullptr : cached->second.lock();
                    if (!process || process->exit_status().status || entry->second <= this->now())
                    {
                        if (!process && cached != this->opened_processes.end())
                        {
                            this->opened_processes.erase(cached);
                        }
                        entry = this->watched_processes.erase(entry);
                    }
                    else
                    {
                        request.process_ids.push_back(entry->first);
                        ++entry;
                    }
                }
                const auto discards = this->pending_discards;
                const auto exits = this->pending_exits;
                lock.unlock();
                bool retry_failed{};
                for (const auto& [process_id, exit] : exits)
                {
                    if (this->report_exit(process_id, exit) == process_error::communication_failure)
                    {
                        retry_failed = true;
                    }
                }
                for (const auto process_id : discards)
                {
                    ProcessProtocol::SessionRequestT discard{};
                    discard.operation = ProcessProtocol::SessionOperation_Discard;
                    discard.process_id = process_id;
                    const auto result = this->exchange(std::move(discard));
                    if (result.error == ProcessProtocol::ProcessError_None || result.error == ProcessProtocol::ProcessError_InvalidProcess)
                    {
                        const std::scoped_lock pending(this->mutex);
                        this->pending_discards.erase(process_id);
                    }
                    else
                    {
                        retry_failed = true;
                    }
                }
                ProcessProtocol::SessionResponseT response{};
                if (!request.process_ids.empty())
                {
                    response = this->exchange(std::move(request));
                }
                if (response.error == ProcessProtocol::ProcessError_None)
                {
                    for (const auto& record : response.processes)
                    {
                        this->track(*record);
                    }
                }
                lock.lock();
                for (const auto& process_id : response.missing_process_ids)
                {
                    const auto cached = this->opened_processes.find(process_id);
                    if (cached != this->opened_processes.end())
                    {
                        if (const auto process = cached->second.lock())
                        {
                            process->update_status({.error = process_error::invalid_process});
                        }
                    }
                    this->watched_processes.erase(process_id);
                }
                interval = response.error == ProcessProtocol::ProcessError_None && !retry_failed
                               ? std::chrono::milliseconds(25)
                               : std::min(interval * 2, std::chrono::milliseconds(1000));
                this->work_requested.wait_for(lock, interval, [this] { return this->stopping.load(); });
            }
        }
    };

    process_session::process_session(process_session_endpoint remote, clock_source now, std::chrono::milliseconds request_timeout)
        : implementation_(std::make_unique<implementation>(std::move(remote), std::move(now), request_timeout))
    {
    }

    process_session::~process_session() = default;

    process_session_endpoint process_session::endpoint() const
    {
        return this->implementation_->endpoint;
    }

    bool process_session::owns_registry() const
    {
        return this->implementation_->worker.joinable();
    }

    process_session_reservation_result process_session::reserve_process(const uint32_t process_id, const uint32_t initial_thread_id)
    {
        ProcessProtocol::SessionRequestT request{};
        request.operation = ProcessProtocol::SessionOperation_Reserve;
        request.thread_id = initial_thread_id;
        request.process_id = process_id;
        auto response = this->implementation_->exchange(std::move(request));
        if (response.error == ProcessProtocol::ProcessError_None && (response.process_id == 0 || response.thread_id == 0))
        {
            return {.error = process_error::communication_failure};
        }
        return {.process_id = response.process_id, .thread_id = response.thread_id, .error = decode_error(response.error)};
    }

    guest_id_result process_session::allocate_thread_id(const uint32_t process_id, const uint32_t requested_id)
    {
        ProcessProtocol::SessionRequestT request{};
        request.operation = ProcessProtocol::SessionOperation_AllocateThread;
        request.process_id = process_id;
        request.thread_id = requested_id;
        const auto response = this->implementation_->exchange(std::move(request));
        if (response.error == ProcessProtocol::ProcessError_None && response.thread_id == 0)
        {
            return {.error = process_error::communication_failure};
        }
        return {.id = response.thread_id, .error = decode_error(response.error)};
    }

    process_error process_session::discard_process(const uint32_t process_id)
    {
        ProcessProtocol::SessionRequestT request{};
        request.operation = ProcessProtocol::SessionOperation_Discard;
        request.process_id = process_id;
        const auto error = decode_error(this->implementation_->exchange(std::move(request)).error);
        if (error == process_error::communication_failure)
        {
            const std::scoped_lock lock(this->implementation_->mutex);
            this->implementation_->pending_discards.insert(process_id);
            this->implementation_->work_requested.notify_one();
        }
        return error == process_error::invalid_process ? process_error::none : error;
    }

    process_session_lookup_result process_session::register_process(process_identity identity, const uint32_t initial_thread_id)
    {
        const auto reservation = this->reserve_process(identity.process_id, initial_thread_id);
        if (!reservation)
        {
            return {.error = reservation.error};
        }
        identity.process_id = reservation.process_id;
        auto result = this->publish_process(identity);
        if (!result)
        {
            (void)this->discard_process(identity.process_id);
        }
        return result;
    }

    process_session_lookup_result process_session::publish_process(const process_identity identity)
    {
        ProcessProtocol::SessionRequestT request{};
        request.operation = ProcessProtocol::SessionOperation_Publish;
        request.process_id = identity.process_id;
        request.update = std::make_unique<ProcessProtocol::SessionProcessT>();
        request.update->process_id = identity.process_id;
        request.update->parent_process_id = identity.parent_process_id;
        request.update->native_environment = identity.native_environment;
        request.update->compatibility_environment = identity.compatibility_environment;
        auto result = this->implementation_->lookup(request);
        if (result.error == process_error::communication_failure)
        {
            result = this->implementation_->lookup(std::move(request));
        }
        return result;
    }

    process_session_lookup_result process_session::lookup(const uint32_t process_id) const
    {
        ProcessProtocol::SessionRequestT request{};
        request.process_id = process_id;
        return this->implementation_->lookup(std::move(request));
    }

    process_exit_status_result process_session::exit_status(const std::shared_ptr<session_process>& process) const
    {
        const auto cached = process->exit_status();
        if (cached.status)
        {
            return cached;
        }
        ProcessProtocol::SessionRequestT request{};
        request.process_id = process->identity().process_id;
        const auto result = this->implementation_->lookup(std::move(request));
        return result ? result.process->exit_status() : process_exit_status_result{.error = result.error};
    }

    process_exit_status_result process_session::poll_exit_status(const std::shared_ptr<session_process>& process) const
    {
        const auto cached = process->exit_status();
        if (cached.status || cached.error == process_error::invalid_process || this->owns_registry())
        {
            return cached;
        }
        {
            const std::scoped_lock lock(this->implementation_->mutex);
            const auto current = process->exit_status();
            if (current.status || current.error == process_error::invalid_process)
            {
                return current;
            }
            this->implementation_->watched_processes[process->identity().process_id] = this->implementation_->now() + watch_lifetime;
            this->implementation_->work_requested.notify_one();
        }
        return process->exit_status();
    }

    process_error process_session::mark_exit(const uint32_t process_id, const process_exit& exit)
    {
        return this->implementation_->report_exit(process_id, exit);
    }

}
