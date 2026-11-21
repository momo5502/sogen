#pragma once

#include "symbol_events.hpp"
#include <platform/pdb_signature.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace sogen::symbols
{
    struct pdb_symbol_options;

    class symbol_server
    {
      public:
        enum class source
        {
            cache,
            symbol_store,
            download,
        };

        struct result
        {
            std::filesystem::path path{};
            source origin{};
            bool temporary{};
        };

        static std::optional<result> find(const pdb_signature& sig, const pdb_symbol_options& options,
                                          const symbol_event_callback& on_event, std::string_view module_name);

        symbol_server() = delete;
    };

} // namespace sogen::symbols
