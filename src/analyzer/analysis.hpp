#pragma once

#include <set>
#include <string>
#include "disassembler.hpp"

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
    bool instruction_summary{false};
    bool skip_syscalls{false};
    bool reproducible{false};

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

    disassembler d{};
    std::unordered_map<uint32_t, uint64_t> instructions{};
    std::vector<accessed_import> accessed_imports{};
    std::set<uint64_t> rdtsc_cache{};
    std::set<uint64_t> rdtscp_cache{};
    std::set<std::pair<uint64_t, uint32_t>> cpuid_cache{};
};

void register_analysis_callbacks(analysis_context& c);
std::optional<mapped_module*> get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);
