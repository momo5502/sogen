#pragma once

#include "symbol_source.hpp"

namespace sogen
{
    class windows_emulator;

    class map_symbol_source
    {
      public:
        map_symbol_source(windows_emulator& win_emu, std::vector<map_symbol_input> inputs);

        loaded_symbols load(const mapped_module& mod) const;

      private:
        windows_emulator* win_emu_{};
        std::vector<map_symbol_input> inputs_{};
    };

} // namespace sogen
