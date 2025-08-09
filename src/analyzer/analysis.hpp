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

struct accessed_import
{
    uint64_t address{};
    uint32_t thread_id{};
    uint64_t access_rip{};
    uint64_t access_inst_count{};
    std::string accessor_module{};
    std::string import_name{};
    std::string import_module{};
};

struct analysis_context
{
    const analysis_settings* settings{};
    windows_emulator* win_emu{};

    std::string output{};
    bool has_reached_main{false};

    std::vector<accessed_import> accessed_imports{};
};

void register_analysis_callbacks(analysis_context& c);
mapped_module* get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);
