#pragma once

#include "symbol_source.hpp"
#include "std_include.hpp"
#include <utils/function.hpp>

namespace sogen
{
    enum class symbol_use_mode
    {
        trace,
        callers,
        all,
    };

    class map_symbol_source;
    class pdb_symbol_source;
    class windows_emulator;

    struct symbol_options
    {
        bool verbose_logging{};
        symbol_use_mode use_mode{symbol_use_mode::callers};
        pdb_symbol_options pdb{};
        std::vector<map_symbol_input> maps{};
        std::set<std::string, std::less<>> interesting_modules{};

        bool use_for_trace() const
        {
            return this->use_mode == symbol_use_mode::trace || this->use_mode == symbol_use_mode::all;
        }

        bool use_for_callers() const
        {
            return this->use_mode == symbol_use_mode::callers || this->use_mode == symbol_use_mode::all;
        }

        bool enabled() const
        {
            return this->pdb.enabled() || !this->maps.empty();
        }
    };

    class symbol_loader
    {
      public:
        symbol_loader(windows_emulator& win_emu, symbol_options options);
        ~symbol_loader();

        symbol_loader(const symbol_loader&) = delete;
        symbol_loader(symbol_loader&&) = delete;
        symbol_loader& operator=(const symbol_loader&) = delete;
        symbol_loader& operator=(symbol_loader&&) = delete;

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
        symbol_options options_{};
        std::unique_ptr<pdb_symbol_source> pdb_{};
        std::unique_ptr<map_symbol_source> map_{};
        utils::callback_id_type module_load_callback_{};
        utils::callback_id_type module_unload_callback_{};
        std::set<uint64_t> attempted_modules_{};

        void load_existing_modules();
        void load_for_module(mapped_module& mod, bool for_callers);
    };

} // namespace sogen
