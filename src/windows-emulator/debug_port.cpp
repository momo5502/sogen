#include "std_include.hpp"
#include "debug_port.hpp"
#include "windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        // Debugger requests read and write guest state through vCPU 0, so that vCPU owns them.
        constexpr size_t debugger_vcpu_index = 0;
    }

    bool debug_port::is_attached() const
    {
        const std::scoped_lock lock(this->mutex_);
        return this->attached_;
    }

    void debug_port::attach()
    {
        const std::scoped_lock lock(this->mutex_);
        this->attached_ = true;
    }

    void debug_port::detach()
    {
        {
            const std::scoped_lock lock(this->mutex_);
            this->attached_ = false;
            this->report_order_.clear();
            for (auto& stopped : this->stopped_ | std::views::values)
            {
                stopped.undelivered.reset();
            }
        }
        this->condition_.notify_all();
    }

    void debug_port::begin_emulation()
    {
        const std::scoped_lock lock(this->mutex_);
        this->emulating_ = true;
    }

    void debug_port::end_emulation()
    {
        std::unique_lock lock(this->mutex_);
        this->emulating_ = false;
        this->run_tasks(lock);
    }

    void debug_port::run_tasks(std::unique_lock<std::mutex>& lock)
    {
        while (!this->tasks_.empty())
        {
            auto task = std::move(this->tasks_.front());
            this->tasks_.pop_front();
            lock.unlock();
            task.run();
            task.done->set_value();
            lock.lock();
        }
    }

    void debug_port::run_pending_tasks(const size_t vcpu_index)
    {
        if (vcpu_index != debugger_vcpu_index)
        {
            return;
        }

        std::unique_lock lock(this->mutex_);
        this->run_tasks(lock);
    }

    uint32_t debug_port::report(windows_emulator& win_emu, const size_t vcpu_index, process_debug_event event)
    {
        const auto thread_id = event.thread_id;

        {
            const std::scoped_lock lock(this->mutex_);
            if (!this->attached_)
            {
                return DBG_CONTINUE;
            }

            auto& stopped = this->stopped_[thread_id];
            stopped.continue_status.reset();
            stopped.undelivered = std::move(event);
            stopped.vcpu_index = vcpu_index;
            this->report_order_.push_back(thread_id);
        }

        return win_emu.run_without_kernel_lock([this, thread_id, vcpu_index] {
            std::unique_lock lock(this->mutex_);

            const auto continued = [this, thread_id] {
                const auto entry = this->stopped_.find(thread_id);
                return entry == this->stopped_.end() || entry->second.continue_status.has_value();
            };

            // Only the vCPU a debugger request is run against may execute one, so a reporter parked on any other
            // vCPU just waits: its host thread must not touch vCPU 0's registers while vCPU 0 runs guest code.
            const auto runs_tasks = vcpu_index == debugger_vcpu_index;

            for (;;)
            {
                this->condition_.wait(lock, [&] { return continued() || (runs_tasks && !this->tasks_.empty()) || !this->attached_; });

                if (runs_tasks)
                {
                    this->run_tasks(lock);
                }

                if (continued() || !this->attached_)
                {
                    break;
                }
            }

            auto status = static_cast<uint32_t>(DBG_CONTINUE);
            if (const auto entry = this->stopped_.find(thread_id); entry != this->stopped_.end())
            {
                status = entry->second.continue_status.value_or(status);
                this->stopped_.erase(entry);
            }

            std::erase(this->report_order_, thread_id);
            return status;
        });
    }

    std::optional<process_debug_event> debug_port::take_event()
    {
        const std::scoped_lock lock(this->mutex_);

        while (!this->report_order_.empty())
        {
            const auto thread_id = this->report_order_.front();
            this->report_order_.pop_front();

            const auto entry = this->stopped_.find(thread_id);
            if (entry != this->stopped_.end() && entry->second.undelivered)
            {
                return std::exchange(entry->second.undelivered, std::nullopt);
            }
        }

        return std::nullopt;
    }

    bool debug_port::continue_event(const uint32_t thread_id, const uint32_t status)
    {
        {
            const std::scoped_lock lock(this->mutex_);
            const auto entry = this->stopped_.find(thread_id);
            if (entry == this->stopped_.end() || entry->second.undelivered || entry->second.continue_status)
            {
                return false;
            }
            entry->second.continue_status = status;
        }
        this->condition_.notify_all();
        return true;
    }

    void debug_port::run_on_emulation_thread(windows_emulator& win_emu, const std::function<void()>& task)
    {
        std::promise<void> done{};
        auto finished = done.get_future();
        bool stopped{};
        {
            const std::scoped_lock lock(this->mutex_);
            if (!this->emulating_)
            {
                win_emu.synchronize(task);
                return;
            }

            this->tasks_.push_back({.run = [&] { win_emu.synchronize(task); }, .done = &done});

            // The task runs on vCPU 0, so it is woken: either it is parked in a report and runs the task from
            // there, or its emulation loop has to be interrupted to reach run_pending_tasks.
            stopped = std::ranges::any_of(this->stopped_ | std::views::values,
                                          [](const stopped_thread& entry) { return entry.vcpu_index == debugger_vcpu_index; });
        }

        if (stopped)
        {
            this->condition_.notify_all();
        }
        else
        {
            win_emu.interrupt_for_debugger();
        }

        finished.wait();
    }
}
