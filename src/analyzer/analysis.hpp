#pragma once

#include <set>
#include <string>

struct mapped_module;
class module_manager;
class windows_emulator;

using string_set = std::set<std::string, std::less<>>;

struct analysis_settings
{
    bool concise_logging{false};
    bool verbose_logging{false};
    bool silent{false};
    bool buffer_stdout{false};

    string_set modules{};
    string_set ignored_functions{};
};

struct analysis_context
{
    const analysis_settings* settings{};
    windows_emulator* win_emu{};

    std::string output{};
    bool has_reached_main{false};
};

void register_analysis_callbacks(analysis_context& c);
mapped_module* get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);