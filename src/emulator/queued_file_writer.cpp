#include "queued_file_writer.hpp"

#include <chrono>

namespace sogen
{

    namespace
    {
        // The writer is not woken per line: waking it costs the logging thread a host call, and verbose runs log tens of
        // thousands of lines a second while holding the kernel lock.
        constexpr auto write_interval = std::chrono::milliseconds(50);
        constexpr size_t max_queued_bytes = 64ULL * 1024 * 1024;
    }

    queued_file_writer::queued_file_writer(FILE* stream)
        : stream_(stream),
          thread_([this] { this->write_queue(); })
    {
    }

    queued_file_writer::~queued_file_writer()
    {
        {
            const std::scoped_lock lock{this->mutex_};
            this->stopping_ = true;
        }

        this->wake_writer_.notify_one();
        this->thread_.join();
    }

    void queued_file_writer::write(const std::string_view text, const bool wait_until_written)
    {
        std::unique_lock lock{this->mutex_};

        if (this->queue_.size() >= max_queued_bytes)
        {
            this->request_write();
            this->written_.wait(lock, [this] { return this->queue_.size() < max_queued_bytes; });
        }

        this->queue_.append(text);
        this->queued_bytes_ += text.size();

        if (wait_until_written)
        {
            this->wait_for_writer(lock);
        }
    }

    void queued_file_writer::flush()
    {
        std::unique_lock lock{this->mutex_};
        this->wait_for_writer(lock);
    }

    void queued_file_writer::request_write()
    {
        this->write_requested_ = true;
        this->wake_writer_.notify_one();
    }

    void queued_file_writer::wait_for_writer(std::unique_lock<std::mutex>& lock)
    {
        const auto end = this->queued_bytes_;
        if (this->written_bytes_ < end)
        {
            this->request_write();
            this->written_.wait(lock, [&] { return this->written_bytes_ >= end; });
        }
    }

    void queued_file_writer::write_queue()
    {
        std::string batch{};
        std::unique_lock lock{this->mutex_};

        while (!this->stopping_ || !this->queue_.empty())
        {
            this->wake_writer_.wait_for(lock, write_interval, [this] { return this->write_requested_ || this->stopping_; });
            this->write_requested_ = false;

            if (this->queue_.empty())
            {
                continue;
            }

            batch.swap(this->queue_);
            const auto end = this->queued_bytes_;
            lock.unlock();

            (void)fwrite(batch.data(), 1, batch.size(), this->stream_);
            (void)fflush(this->stream_);
            batch.clear();

            lock.lock();
            this->written_bytes_ = end;
            this->written_.notify_all();
        }
    }

} // namespace sogen
