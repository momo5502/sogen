#pragma once

#include "symbol_source.hpp"

namespace sogen
{
    class windows_emulator;

    class pdb_symbol_source
    {
      public:
        pdb_symbol_source(windows_emulator& win_emu, pdb_symbol_options options);

        loaded_symbols load(const mapped_module& mod) const;

      private:
        windows_emulator* win_emu_{};
        pdb_symbol_options options_{};
    };

} // namespace sogen
