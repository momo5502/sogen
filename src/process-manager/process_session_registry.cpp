#include "process_session_registry.hpp"
#include <algorithm>

namespace sogen
{
    process_session_registry::process_session_registry(process_session::clock_source clock)
        : now(std::move(clock))
    {
    }

    void process_session_registry::expire_reservations()
    {
        const auto current_time = this->now();
        while (!this->reservations.empty())
        {
            const auto entry = this->processes.find(this->reservations.front());
            if (entry->second.reserved_until > current_time)
            {
                break;
            }
            this->processes.erase(entry);
            this->reservations.pop_front();
        }
    }

    ProcessProtocol::SessionResponseT process_session_registry::handle(const ProcessProtocol::SessionRequestT& request)
    {
        const std::scoped_lock lock(this->mutex);
        this->expire_reservations();
        ProcessProtocol::SessionResponseT response{};
        if (request.operation == ProcessProtocol::SessionOperation_Reserve)
        {
            if (this->reservations.size() >= 65536)
            {
                response.error = ProcessProtocol::ProcessError_ResourceLimit;
                return response;
            }
            // Automatically assigned process IDs leave PID + 4 for the initial thread.
            auto process_id = request.process_id;
            if (process_id == 0)
            {
                while (this->next_process_id <= UINT32_MAX - 4 &&
                       (this->allocated_ids.contains(static_cast<uint32_t>(this->next_process_id)) ||
                        this->allocated_ids.contains(static_cast<uint32_t>(this->next_process_id + 4)) ||
                        this->next_process_id == request.initial_thread_id))
                {
                    this->next_process_id += 8;
                }
                if (this->next_process_id > UINT32_MAX - 4)
                {
                    response.error = ProcessProtocol::ProcessError_ResourceLimit;
                    return response;
                }
                process_id = static_cast<uint32_t>(this->next_process_id);
                this->next_process_id += 8;
            }
            if (process_id % 4 != 0 || (!request.initial_thread_id && process_id > UINT32_MAX - 4))
            {
                response.error = ProcessProtocol::ProcessError_Unavailable;
                return response;
            }
            const auto thread_id = request.initial_thread_id ? request.initial_thread_id : process_id + 4;
            if (thread_id % 4 != 0 || thread_id == process_id || this->allocated_ids.contains(process_id) ||
                this->allocated_ids.contains(thread_id))
            {
                response.error = ProcessProtocol::ProcessError_Unavailable;
                return response;
            }
            this->allocated_ids.insert(process_id);
            this->allocated_ids.insert(thread_id);
            response.process_id = process_id;
            auto& record = this->processes[process_id];
            record.process.process_id = process_id;
            // The launch handshake has a 30-second deadline; this lease also allows time for registry exchanges.
            record.reserved_until = this->now() + reservation_lifetime;
            this->reservations.push_back(response.process_id);
            record.reservation = std::prev(this->reservations.end());
            return response;
        }
        if (request.operation == ProcessProtocol::SessionOperation_Refresh)
        {
            for (const auto& process_id : request.process_ids)
            {
                const auto entry = this->processes.find(process_id);
                if (entry == this->processes.end() || !entry->second.published || entry->second.discarded)
                {
                    response.missing_process_ids.push_back(process_id);
                    continue;
                }
                response.processes.push_back(std::make_unique<ProcessProtocol::SessionProcessT>(entry->second.process));
            }
            return response;
        }
        const auto entry = this->processes.find(request.process_id);
        if (entry == this->processes.end())
        {
            if (request.operation != ProcessProtocol::SessionOperation_Discard)
            {
                response.error = ProcessProtocol::ProcessError_InvalidProcess;
            }
            return response;
        }
        auto& record = entry->second;
        if (request.operation == ProcessProtocol::SessionOperation_Discard)
        {
            if (record.published)
            {
                record.discarded = true;
            }
            else
            {
                this->reservations.erase(record.reservation);
                this->processes.erase(entry);
            }
            return response;
        }
        if (request.operation == ProcessProtocol::SessionOperation_Publish || request.operation == ProcessProtocol::SessionOperation_Exit)
        {
            if (!request.update || request.update->process_id != request.process_id)
            {
                response.error = ProcessProtocol::ProcessError_InvalidProcess;
                return response;
            }
            if (request.operation == ProcessProtocol::SessionOperation_Publish)
            {
                if (record.discarded)
                {
                    response.error = ProcessProtocol::ProcessError_Unavailable;
                    return response;
                }
                if (record.published)
                {
                    const auto& previous = record.process;
                    const auto& update = *request.update;
                    if (previous.parent_process_id != update.parent_process_id ||
                        previous.native_environment != update.native_environment ||
                        previous.compatibility_environment != update.compatibility_environment)
                    {
                        response.error = ProcessProtocol::ProcessError_Unavailable;
                        return response;
                    }
                }
                else
                {
                    record.process.process_id = request.process_id;
                    record.process.parent_process_id = request.update->parent_process_id;
                    record.process.native_environment = request.update->native_environment;
                    record.process.compatibility_environment = request.update->compatibility_environment;
                    this->reservations.erase(record.reservation);
                    record.published = true;
                }
            }
            // Guest exit and the host monitor race; the first terminal report for this process_id wins.
            else if (!record.process.exited)
            {
                record.process.process_id = request.process_id;
                record.process.exited = true;
                record.process.exit_code = request.update->exit_code;
                record.process.exit_kind = request.update->exit_kind;
                record.process.cycles = request.update->cycles;
            }
            else if (record.process.exit_code == request.update->exit_code && record.process.exit_kind == request.update->exit_kind)
            {
                record.process.cycles = std::max(record.process.cycles, request.update->cycles);
            }
        }
        else if (!record.published || record.discarded)
        {
            response.error = ProcessProtocol::ProcessError_InvalidProcess;
            return response;
        }
        response.process = std::make_unique<ProcessProtocol::SessionProcessT>(record.process);
        return response;
    }

}
