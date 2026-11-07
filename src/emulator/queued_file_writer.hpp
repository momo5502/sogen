#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace sogen
{

    class queued_file_writer
    {
      public:
        explicit queued_file_writer(FILE* stream);
        ~queued_file_writer();

        queued_file_writer(const queued_file_writer&) = delete;
        queued_file_writer& operator=(const queued_file_writer&) = delete;

        void write(std::string_view text, bool wait_until_written = false);

      private:
        FILE* stream_{};
        std::mutex mutex_{};
        std::condition_variable wake_writer_{};
        std::condition_variable written_{};
        std::string queue_{};
        uint64_t queued_bytes_{};
        uint64_t written_bytes_{};
        bool write_requested_{false};
        bool stopping_{false};
        std::thread thread_{};

        void request_write();
        void write_queue();
    };

} // namespace sogen
