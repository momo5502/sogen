#pragma once

#include "symbol_source.hpp"
#include "symbol_events.hpp"
#include <mutex>

namespace sogen::symbols
{
    class pdb_symbol_source
    {
      public:
        pdb_symbol_source(windows_emulator& win_emu, pdb_symbol_options options);

        symbol_event_callback on_event{};

        loaded_symbols load(const mapped_module& mod) const;

      private:
        windows_emulator* win_emu_{};
        pdb_symbol_options options_{};
        mutable std::once_flag cache_initialized_{};
    };

} // namespace sogen::symbols
