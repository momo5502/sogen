#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sogen
{

    class queued_file_writer
    {
      public:
        // Appends the text of a deferred line to out.
        using format_function = void (*)(const void* record, std::string& out);

        explicit queued_file_writer(FILE* stream);
        ~queued_file_writer();

        queued_file_writer(const queued_file_writer&) = delete;
        queued_file_writer& operator=(const queued_file_writer&) = delete;

        void write(std::string_view text, bool wait_until_written = false);
        // Returns once everything written before the call has reached the stream.
        void flush();

        // Queues a line that the writer thread formats from a copy of record, in order with the written text.
        void write_deferred(format_function format, const void* record, size_t size);

      private:
        struct deferred_line
        {
            size_t text_offset{};
            format_function format{};
            size_t record_offset{};
        };

        FILE* stream_{};
        std::mutex mutex_{};
        std::condition_variable wake_writer_{};
        std::condition_variable written_{};
        std::string queue_{};
        std::vector<deferred_line> deferred_{};
        std::string records_{};
        // Positions in the output: a byte of text or a deferred line each count one.
        uint64_t queued_bytes_{};
        uint64_t written_bytes_{};
        bool write_requested_{false};
        bool stopping_{false};
        std::thread thread_{};

        void wait_for_room(std::unique_lock<std::mutex>& lock);
        void request_write();
        void wait_for_writer(std::unique_lock<std::mutex>& lock);
        void write_queue();
    };

} // namespace sogen
