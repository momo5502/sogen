#pragma once

#include "symbol_source.hpp"
#include "symbol_events.hpp"

namespace sogen::symbols
{
    class map_symbol_source
    {
      public:
        map_symbol_source(windows_emulator& win_emu, std::vector<map_symbol_input> inputs);

        symbol_event_callback on_event{};

        loaded_symbols load(const mapped_module& mod) const;

      private:
        windows_emulator* win_emu_{};
        std::vector<map_symbol_input> inputs_{};
    };

} // namespace sogen::symbols
