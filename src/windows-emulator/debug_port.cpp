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
            this->undelivered_.reset();
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
        {
            const std::scoped_lock lock(this->mutex_);
            if (!this->attached_)
            {
                return DBG_CONTINUE;
            }

            this->stopped_thread_ = event.thread_id;
            this->continue_status_.reset();
            this->undelivered_ = std::move(event);
        }

        return win_emu.run_without_kernel_lock([this] {
            std::unique_lock lock(this->mutex_);
            for (;;)
            {
                this->condition_.wait(lock, [this] { return this->continue_status_ || !this->tasks_.empty() || !this->attached_; });
                this->run_tasks(lock);

                if (this->continue_status_ || !this->attached_)
                {
                    break;
                }
            }

            this->stopped_thread_.reset();
            return std::exchange(this->continue_status_, std::nullopt).value_or(static_cast<uint32_t>(DBG_CONTINUE));
        });
    }

    std::optional<process_debug_event> debug_port::take_event()
    {
        const std::scoped_lock lock(this->mutex_);
        return std::exchange(this->undelivered_, std::nullopt);
    }

    bool debug_port::continue_event(const uint32_t thread_id, const uint32_t status)
    {
        {
            const std::scoped_lock lock(this->mutex_);
            if (!this->stopped_thread_ || *this->stopped_thread_ != thread_id || this->undelivered_)
            {
                return false;
            }
            this->continue_status_ = status;
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
            stopped = this->stopped_thread_.has_value();
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
