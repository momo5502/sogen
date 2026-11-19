#pragma once

#include <platform/pdb_signature.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace sogen
{
    class logger;
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

        static std::optional<result> find(const pdb_signature& sig, const pdb_symbol_options& options, const logger& log,
                                          std::string_view module_name);

        symbol_server() = delete;
    };

} // namespace sogen
