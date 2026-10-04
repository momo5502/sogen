#pragma once

#include "macos_analysis_event.hpp"

#include <logger.hpp>

namespace sogen
{
    struct macos_console_reporter_settings
    {
        bool silent{};
        bool concise{};
        bool prepend_call_count{};
    };

    class macos_analysis_reporter
    {
      public:
        virtual ~macos_analysis_reporter() = default;

        macos_analysis_reporter() = default;
        macos_analysis_reporter(const macos_analysis_reporter&) = delete;
        macos_analysis_reporter& operator=(const macos_analysis_reporter&) = delete;
        macos_analysis_reporter(macos_analysis_reporter&&) = delete;
        macos_analysis_reporter& operator=(macos_analysis_reporter&&) = delete;

        virtual void report(const macos_analysis_event& event) = 0;

        virtual void flush()
        {
        }
    };

    std::unique_ptr<macos_analysis_reporter> create_macos_console_reporter(logger& log, macos_console_reporter_settings settings);
}
