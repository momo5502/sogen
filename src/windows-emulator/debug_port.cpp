#include "std_include.hpp"
#include "debug_port.hpp"
#include "windows_emulator.hpp"

namespace sogen
{
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

    void debug_port::run_pending_tasks()
    {
        std::unique_lock lock(this->mutex_);
        this->run_tasks(lock);
    }

    uint32_t debug_port::report(windows_emulator& win_emu, process_debug_event event)
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
            this->report_order_.push_back(thread_id);
        }

        return win_emu.run_without_kernel_lock([this, thread_id] {
            std::unique_lock lock(this->mutex_);

            const auto continued = [this, thread_id] {
                const auto entry = this->stopped_.find(thread_id);
                return entry == this->stopped_.end() || entry->second.continue_status.has_value();
            };

            for (;;)
            {
                this->condition_.wait(lock, [&] { return continued() || !this->tasks_.empty() || !this->attached_; });
                this->run_tasks(lock);

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
            stopped = !this->stopped_.empty();
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
