#pragma once

#include <platform/pdb_signature.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace sogen
{
    class logger;
    struct pdb_symbol_options;

    enum class symbol_server_source
    {
        cache,
        symbol_store,
        download,
    };

    struct symbol_server_result
    {
        std::filesystem::path path{};
        symbol_server_source source{};
        bool temporary{};
    };

    std::optional<symbol_server_result> find_on_symbol_servers(const pdb_signature& sig, const pdb_symbol_options& options,
                                                               const logger& log, std::string_view module_name);

} // namespace sogen
