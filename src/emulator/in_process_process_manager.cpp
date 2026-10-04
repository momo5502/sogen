#include "in_process_process_manager.hpp"

#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace sogen
{
    struct in_process_process_manager::process_entry
    {
        std::unique_ptr<in_process_process> process{};
        std::jthread thread{};
        mutable std::mutex mutex{};
        std::optional<process_exit> exit{};
    };

    in_process_process_manager::in_process_process_manager(process_factory factory)
        : factory_(std::move(factory))
    {
        if (!this->factory_)
        {
            throw std::invalid_argument("A process factory is required");
        }
    }

    in_process_process_manager::~in_process_process_manager()
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
            try
            {
                process->process->terminate(0);
            }
            catch (...)
            {
            }
        }

        for (const auto& process : processes)
        {
            if (process->thread.joinable())
            {
                process->thread.join();
            }
        }
    }

    process_create_result in_process_process_manager::create_process(process_create_request request)
    {
        const std::scoped_lock lock(this->mutex_);
        if (this->stopping_)
        {
            return {{}, process_error::unavailable};
        }

        std::unique_ptr<in_process_process> process{};
        try
        {
            process = this->factory_(std::move(request));
        }
        catch (...)
        {
            return {{}, process_error::internal_failure};
        }

        if (!process)
        {
            return {{}, process_error::unavailable};
        }

        const auto entry = std::make_shared<process_entry>();
        entry->process = std::move(process);

        if (this->next_process_ == 0)
        {
            return {{}, process_error::resource_limit};
        }

        const managed_process handle{this->next_process_++};
        this->processes_.emplace(handle.value, entry);

        try
        {
            entry->thread = std::jthread([entry] {
                process_exit exit{};
                try
                {
                    exit = entry->process->run();
                }
                catch (...)
                {
                    exit = {.kind = process_exit_kind::runtime_failure};
                }

                const std::scoped_lock lock(entry->mutex);
                entry->exit = exit;
            });
        }
        catch (...)
        {
            this->processes_.erase(handle.value);
            return {{}, process_error::resource_limit};
        }

        return {handle, process_error::none};
    }

    process_error in_process_process_manager::terminate_process(const managed_process process, const uint64_t exit_code)
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return process_error::invalid_process;
        }

        try
        {
            entry->process->terminate(exit_code);
            return process_error::none;
        }
        catch (...)
        {
            return process_error::internal_failure;
        }
    }

    process_exit_status_result in_process_process_manager::exit_status(const managed_process process) const
    {
        const auto entry = this->find_process(process);
        if (!entry)
        {
            return {{}, process_error::invalid_process};
        }

        const std::scoped_lock lock(entry->mutex);
        return {entry->exit, process_error::none};
    }

    std::shared_ptr<in_process_process_manager::process_entry> in_process_process_manager::find_process(const managed_process process) const
    {
        const std::scoped_lock lock(this->mutex_);
        const auto entry = this->processes_.find(process.value);
        return entry == this->processes_.end() ? nullptr : entry->second;
    }
}
