#pragma once

#include "analysis_event.hpp"

#include <filesystem>
#include <memory>

class logger;

struct console_reporter_settings
{
    bool silent{};
    bool buffer_stdout{};
};

class analysis_reporter
{
  public:
    virtual ~analysis_reporter() = default;
    virtual void report(const analysis_event& event) = 0;
    virtual void flush()
    {
    }
};

std::unique_ptr<analysis_reporter> create_console_reporter(logger& log, console_reporter_settings settings);
std::unique_ptr<analysis_reporter> create_jsonl_reporter(const std::filesystem::path& path);
