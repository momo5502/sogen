#pragma once

#include "std_include.hpp"

namespace sogen
{
    struct mapped_module;

    using rva_symbol_mapping = std::map<uint32_t, std::string>;

    struct loaded_symbols
    {
        rva_symbol_mapping address_names{};

        bool empty() const
        {
            return this->address_names.empty();
        }
    };

    struct pdb_symbol_options
    {
        bool auto_lookup{};
        bool verbose_logging{};
        std::vector<std::filesystem::path> inputs{};
        std::vector<std::string> symbol_servers{};
        std::optional<std::filesystem::path> symbol_cache{};

        bool enabled() const
        {
            return this->auto_lookup || !this->inputs.empty();
        }
    };

    struct map_symbol_input
    {
        std::optional<std::string> module{};
        std::filesystem::path path{};
    };

    std::optional<uint32_t> symbol_address_to_rva(const mapped_module& mod, uint32_t section_index, uint32_t offset);

} // namespace sogen
