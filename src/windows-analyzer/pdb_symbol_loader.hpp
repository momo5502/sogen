#pragma once

#include "std_include.hpp"
#include <utils/function.hpp>

namespace sogen
{
    enum class pdb_use_mode
    {
        trace,
        callers,
        all,
    };

    struct mapped_module;
    class windows_emulator;

    struct pdb_symbol_options
    {
        bool auto_lookup{};
        bool verbose_logging{};
        pdb_use_mode use_mode{pdb_use_mode::callers};
        std::vector<std::filesystem::path> inputs{};
        std::vector<std::string> symbol_servers{};
        std::optional<std::filesystem::path> symbol_cache{};
        std::set<std::string, std::less<>> interesting_modules{};

        bool use_for_trace() const
        {
            return this->use_mode == pdb_use_mode::trace || this->use_mode == pdb_use_mode::all;
        }

        bool use_for_callers() const
        {
            return this->use_mode == pdb_use_mode::callers || this->use_mode == pdb_use_mode::all;
        }

        bool enabled() const
        {
            return this->auto_lookup || !this->inputs.empty();
        }
    };

    class pdb_symbol_loader
    {
      public:
        pdb_symbol_loader(windows_emulator& win_emu, pdb_symbol_options options);
        ~pdb_symbol_loader();

        pdb_symbol_loader(const pdb_symbol_loader&) = delete;
        pdb_symbol_loader(pdb_symbol_loader&&) = delete;
        pdb_symbol_loader& operator=(const pdb_symbol_loader&) = delete;
        pdb_symbol_loader& operator=(pdb_symbol_loader&&) = delete;

        bool use_for_trace() const
        {
            return this->options_.use_for_trace();
        }

        bool use_for_callers() const
        {
            return this->options_.use_for_callers();
        }

        void ensure_caller_symbols(uint64_t address);

      private:
        windows_emulator* win_emu_{};
        pdb_symbol_options options_{};
        utils::callback_id_type module_load_callback_{};
        std::set<std::string> loaded_signatures_{};
        std::set<std::string> attempted_loads_{};

        void load_existing_modules();
        void load_for_module(mapped_module& mod, bool for_callers);
    };

} // namespace sogen
