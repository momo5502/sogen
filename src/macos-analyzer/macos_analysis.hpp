#pragma once

#include "std_include.hpp"

#include "macos_reporter.hpp"

#include <map>
#include <set>

#include <macos_emulator.hpp>

namespace sogen
{
    struct macos_analysis_options
    {
        std::filesystem::path emulation_root{};
        std::filesystem::path executable{};
        std::vector<std::string> argv{};
        std::vector<std::string> envp{};
        bool silent{};
        bool verbose{};
        bool concise{};
        bool skip_syscalls{};
        bool skip_arguments{};
        bool prepend_call_count{};
        bool memory_report{};
        size_t string_limit{256};
        size_t buffer_preview_limit{32};
        uint64_t max_instructions{};
        std::set<std::string, std::less<>> ignored_syscalls{};
        std::vector<std::pair<uint64_t, size_t>> memory_dumps{};

        std::filesystem::path screenshot{};

        // Whether the window path is intercepted at all. --screenshot implies it, but the two are
        // different questions: a GUI app needs the interception to get past AppKit's start-up whether or
        // not anyone wants a PNG at the end, and a run that does not enable it reaches the real SkyLight
        // stubs and dies somewhere unrelated.
        bool gui{};

        // Opens a real host window and lets it deliver clicks and keys, instead of composing headlessly.
        // A backend that reports it can deliver input is also what stops an application idling on its run
        // loop being read as a deadlock, so this is what makes a GUI run outlive its first frame.
        bool interactive{};

        int32_t desktop_width{1440};
        int32_t desktop_height{900};

        // The browser has no host mmap to alias, so it always reaches the shared cache through the
        // pager. This makes that path reachable natively, where a run costs minutes instead of an hour.
        bool lazy_cache_paging{};
    };

    class macos_module_index
    {
      public:
        void add(std::string name, uint64_t start, uint64_t size);
        std::string find(uint64_t address) const;

      private:
        struct entry
        {
            uint64_t end{};
            std::string name{};
        };

        std::map<uint64_t, entry> entries_{};
    };

    struct macos_analysis_context
    {
        macos_emulator* emu{};
        macos_analysis_reporter* reporter{};
        const macos_analysis_options* options{};

        uint64_t call_count{};

        // The emulator emits argument rows unconditionally, so a suppressed header has to suppress the
        // rows that belong to it as well, or --ignore would hide a call and still print its arguments.
        bool suppress_details{};
        macos_module_index modules{};

        void emit(const macos_analysis_event& event) const;
        macos_execution_context execution() const;
    };

    void register_macos_callbacks(macos_analysis_context& c);
    std::string macos_permission_string(memory_permission permission);
    int run_macos_analysis(const macos_analysis_options& options, logger& log);
}
