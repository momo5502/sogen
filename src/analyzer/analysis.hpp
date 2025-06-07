#pragma once

#include <set>
#include <string>

class windows_emulator;

struct analysis_settings
{
    bool concise_logging{false};
    bool verbose_logging{false};
    bool silent{false};
    bool buffer_stdout{false};

    std::set<std::string, std::less<>> modules{};
    std::set<std::string, std::less<>> ignored_functions{};
};

struct analysis_context
{
    const analysis_settings* settings{};
    windows_emulator* win_emu{};

    std::string output{};
    bool has_reached_main{false};
};

void register_analysis_callbacks(analysis_context& c);
