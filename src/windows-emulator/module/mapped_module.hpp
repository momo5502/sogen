#pragma once
#include <memory_region.hpp>
#include <serialization.hpp>

struct exported_symbol
{
    std::string name{};
    uint64_t ordinal{};
    uint64_t rva{};
    uint64_t address{};
};

struct imported_symbol
{
    std::string name{};
    size_t module_index{};
};

using exported_symbols = std::vector<exported_symbol>;
using imported_symbols = std::unordered_map<uint64_t, imported_symbol>;
using imported_module_list = std::vector<std::string>;
using address_name_mapping = serialized_map<uint64_t, std::string>;

struct mapped_section
{
    std::string name{};
    basic_memory_region<> region{};
};

struct mapped_module
{
    std::string name{};
    std::filesystem::path path{};

    uint64_t image_base{};
    uint64_t image_base_file{};
    uint64_t size_of_image{};
    uint64_t entry_point{};

    // PE header fields
    uint16_t machine{};               // Machine type from file header
    uint64_t size_of_stack_reserve{}; // Stack reserve size from optional header
    uint64_t size_of_stack_commit{};  // Stack commit size from optional header
    uint64_t size_of_heap_reserve{};  // Heap reserve size from optional header
    uint64_t size_of_heap_commit{};   // Heap commit size from optional header

    exported_symbols exports{};
    imported_symbols imports{};
    imported_module_list imported_modules{};
    address_name_mapping address_names{};

    std::vector<mapped_section> sections{};

    bool is_static{false};

    bool contains(const uint64_t address) const
    {
        return (address - this->image_base) < this->size_of_image;
    }

    uint64_t find_export(const std::string_view export_name) const
    {
        for (const auto& symbol : this->exports)
        {
            if (symbol.name == export_name)
            {
                return symbol.address;
            }
        }

        return 0;
    }

    uint64_t get_image_base_file() const
    {
        return this->image_base_file;
    }
};
