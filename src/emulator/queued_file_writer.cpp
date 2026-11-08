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

    void queued_file_writer::wait_for_room(std::unique_lock<std::mutex>& lock)
    {
        if (this->queue_.size() + this->records_.size() >= max_queued_bytes)
        {
            this->request_write();
            this->written_.wait(lock, [this] { return this->queue_.size() + this->records_.size() < max_queued_bytes; });
        }
    }

    void queued_file_writer::write(const std::string_view text, const bool wait_until_written)
    {
        std::unique_lock lock{this->mutex_};
        this->wait_for_room(lock);

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

    void queued_file_writer::write_deferred(const format_function format, const void* record, const size_t size)
    {
        std::unique_lock lock{this->mutex_};
        this->wait_for_room(lock);

        this->deferred_.push_back({.text_offset = this->queue_.size(), .format = format, .record_offset = this->records_.size()});
        this->records_.append(static_cast<const char*>(record), size);
        ++this->queued_bytes_;
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
        std::vector<deferred_line> deferred{};
        std::string records{};
        std::string output{};
        std::unique_lock lock{this->mutex_};

        while (!this->stopping_ || !this->queue_.empty() || !this->deferred_.empty())
        {
            this->wake_writer_.wait_for(lock, write_interval, [this] { return this->write_requested_ || this->stopping_; });
            this->write_requested_ = false;

            if (this->queue_.empty() && this->deferred_.empty())
            {
                continue;
            }

            batch.swap(this->queue_);
            deferred.swap(this->deferred_);
            records.swap(this->records_);
            const auto end = this->queued_bytes_;
            lock.unlock();

            const auto* text = &batch;
            if (!deferred.empty())
            {
                size_t written_text = 0;
                for (const auto& line : deferred)
                {
                    output.append(batch, written_text, line.text_offset - written_text);
                    line.format(records.data() + line.record_offset, output);
                    written_text = line.text_offset;
                }

                output.append(batch, written_text);
                text = &output;
            }

            (void)fwrite(text->data(), 1, text->size(), this->stream_);
            (void)fflush(this->stream_);
            batch.clear();
            deferred.clear();
            records.clear();
            output.clear();

            lock.lock();
            this->written_bytes_ = end;
            this->written_.notify_all();
        }
    }

} // namespace sogen
