#pragma once

#include <type_traits>
#include <utility>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "analysis_event.hpp"
#include "disassembler.hpp"

struct mapped_module;
class module_manager;
class windows_emulator;
class analysis_reporter;

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
    bool log_first_section_execution{false};

    string_set modules{};
    string_set ignored_functions{};
};

struct accessed_import
{
    uint64_t address{};
    execution_context access_context{};
    uint64_t access_inst_count{};
    std::string import_name{};
    std::string import_module{};
};

struct analysis_context
{
    const analysis_settings* settings{};
    windows_emulator* win_emu{};
    std::vector<analysis_reporter*> reporters{};

    std::string output{};
    bool has_reached_main{false};

    disassembler d{};
    std::unordered_map<uint32_t, uint64_t> instructions{};
    std::vector<accessed_import> accessed_imports{};
    std::set<uint64_t> rdtsc_cache{};
    std::set<uint64_t> rdtscp_cache{};
    std::set<std::pair<uint64_t, uint32_t>> cpuid_cache{};

    mutable std::pair<uint64_t, uint64_t> mapping_violation{0, 0};
    mutable uint64_t next_event_sequence{1};

    event_header make_event_header() const;
    execution_context make_execution_context() const;
    void emit_event(const analysis_event& event) const;

    template <typename Event, typename Initializer>
    void emit_observation(Initializer&& initialize) const
    {
        this->emit_observation<Event>(this->make_execution_context(), std::forward<Initializer>(initialize));
    }

    template <typename Event, typename Initializer>
    void emit_observation(execution_context context, Initializer&& initialize) const
    {
        static_assert(std::is_base_of_v<observation_event, Event>);

        Event event{};
        initialize(event);
        event.header = this->make_event_header();
        event.execution = std::move(context);
        this->emit_event(event);
    }

    template <typename Event>
    void emit_observation() const
    {
        this->emit_observation<Event>([](Event&) {});
    }

    template <typename Event>
    void emit_observation(execution_context context) const
    {
        this->emit_observation<Event>(std::move(context), [](Event&) {});
    }

    template <typename Event, typename Initializer>
    void emit_summary(Initializer&& initialize) const
    {
        static_assert(std::is_base_of_v<summary_event, Event>);

        Event event{};
        initialize(event);
        event.header = this->make_event_header();
        this->emit_event(event);
    }

    template <typename Event>
    void emit_summary() const
    {
        this->emit_summary<Event>([](Event&) {});
    }
};

void register_analysis_callbacks(analysis_context& c);
std::optional<mapped_module*> get_module_if_interesting(module_manager& manager, const string_set& modules, uint64_t address);
