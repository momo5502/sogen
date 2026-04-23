#pragma once
#include "generic_logger.hpp"

#include <functional>
#include <string_view>

class logger : public generic_logger
{
  public:
    // Sink receives every formatted log line before it is written to stdout.
    // The color argument doubles as a level tag: red = error (force-printed),
    // yellow = warn, cyan = info, green = success, gray = log, pink = force-
    // print from external code (e.g. gdb bindings). Sinks run even when
    // output is disabled, so they observe all log activity.
    using sink = std::function<void(color c, std::string_view message)>;

#ifdef OS_WINDOWS
    logger();
    ~logger() override;
#endif
    void print(color c, std::string_view message) override;
    void print(color c, const char* message, ...) override FORMAT_ATTRIBUTE(3, 4);
    void force_print(color c, const char* message, ...) FORMAT_ATTRIBUTE(3, 4);
    void info(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void warn(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void error(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void success(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);
    void log(const char* message, ...) const FORMAT_ATTRIBUTE(2, 3);

    void disable_output(const bool value)
    {
        this->disable_output_ = value;
    }

    bool is_output_disabled() const
    {
        return this->disable_output_;
    }

    // Install a sink callback. Passing an empty std::function clears it.
    // Single-sink: installing replaces any previously installed sink.
    void set_sink(sink s)
    {
        this->sink_ = std::move(s);
    }

  private:
#ifdef OS_WINDOWS
    UINT old_cp{};
#endif
    bool disable_output_{false};
    sink sink_{};
    void print_message(color c, std::string_view message, bool force = false) const;
};
