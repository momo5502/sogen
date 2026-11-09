#pragma once
#include "generic_logger.hpp"

#include <utils/function.hpp>
#include <utils/win.hpp>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace sogen
{

    class queued_file_writer;

    class logger : public generic_logger
    {
      public:
        // Sink receives every formatted log line before it is written to stdout.
        // The color argument doubles as a level tag: red = error (force-printed),
        // yellow = warn, cyan = info, green = success, gray = log, pink = force-
        // print from external code (e.g. gdb bindings). Sinks run even when
        // output is disabled, so they observe all log activity.
        using sink = utils::optional_function<void(color c, std::string_view message)>;

        logger();
        ~logger() override;
        void print(color c, std::string_view message) override;
        void print(color c, const char* message, ...) override FORMAT_ATTRIBUTE(3, 4);
        void force_print(color c, const char* message, ...) FORMAT_ATTRIBUTE(3, 4);
        void info(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
        void warn(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
        void error(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
        void success(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
        void log(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);

        // Returns once every line printed before the call has reached stdout. Redirected output is written by a host thread,
        // so code that writes to stdout directly flushes the logger first to keep its output in order with the log's.
        void flush() const;

        // Prints the line Format(record, out) appends to out. Output redirected to a file or pipe formats it on its
        // writer thread from a copy of record, so a caller logging a line per syscall does not pay for it while it
        // holds the kernel lock; a sink or a terminal gets it formatted at once.
        template <auto Format, typename Record>
            requires std::is_trivially_copyable_v<Record>
        void print_deferred(const color c, const Record& record)
        {
            this->print_deferred_record(c, &format_record<Format, Record>, &record, sizeof(record));
        }

        void disable_output(const bool value)
        {
            this->disable_output_ = value;
        }

        bool is_output_disabled() const
        {
            return this->disable_output_;
        }

        // Fully mute terminal output, including force-printed errors that disable_output() lets through.
        // Sinks still observe everything. Intended for headless consumers (e.g. fuzzing).
        void set_silent(const bool value)
        {
            this->silent_ = value;
        }

        bool is_silent() const
        {
            return this->silent_;
        }

        // Install a sink callback. Passing an empty std::function clears it.
        // Single-sink: installing replaces any previously installed sink.
        void set_sink(sink s)
        {
            this->sink_ = std::move(s);
        }

      private:
#ifdef _WIN32
        UINT old_cp{};
#endif
        bool disable_output_{false};
        bool silent_{false};
        bool terminal_{true};
        sink sink_{};
        mutable std::mutex print_mutex_{};
        mutable std::unique_ptr<queued_file_writer> redirected_output_{};
        void print_message(color c, std::string_view message, bool force = false) const;
        void print_deferred_record(color c, void (*format)(const void* record, std::string& out), const void* record, size_t size);

        template <auto Format, typename Record>
        static void format_record(const void* bytes, std::string& out)
        {
            Record record{};
            std::memcpy(&record, bytes, sizeof(record));
            Format(record, out);
        }
    };

} // namespace sogen
