#pragma once
#include <memory_region.hpp>

struct exported_symbol
{
    std::string name{};
    uint64_t ordinal{};
    uint64_t rva{};
    uint64_t address{};
};

using exported_symbols = std::vector<exported_symbol>;
using address_name_mapping = std::map<uint64_t, std::string>;

struct mapped_section
{
    std::string name{};
    basic_memory_region region{};
};

struct mapped_module
{
    std::string name{};
    std::filesystem::path path{};

    uint64_t image_base{};
    uint64_t size_of_image{};
    uint64_t entry_point{};

    exported_symbols exports{};
    address_name_mapping address_names{};

    std::vector<mapped_section> sections{};

    bool is_within(const uint64_t address) const
    {
        return address >= this->image_base && address < (this->image_base + this->size_of_image);
    }

    uint64_t find_export(const std::string_view export_name) const
    {
        for (auto& symbol : this->exports)
        {
            if (symbol.name == export_name)
            {
                return symbol.address;
            }
        }

        return 0;
    }
};
