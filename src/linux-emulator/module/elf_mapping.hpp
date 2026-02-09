#pragma once

#include "../std_include.hpp"
#include "../linux_memory_manager.hpp"
#include "../linux_emulator_utils.hpp"
#include <platform/elf.hpp>

// Reuse mapped_module from windows-emulator (it's OS-agnostic)
// Re-declare the relevant types here to avoid depending on windows-emulator headers

struct linux_exported_symbol
{
    std::string name{};
    uint64_t rva{};
    uint64_t address{};
};

struct linux_mapped_section
{
    std::string name{};
    uint64_t start{};
    size_t length{};
    memory_permission permissions{};
};

struct linux_mapped_module
{
    std::string name{};
    std::filesystem::path path{};

    uint64_t image_base{};
    uint64_t size_of_image{};
    uint64_t entry_point{};
    uint16_t machine{};
    uint16_t elf_type{};

    // PHDR info for auxv
    uint64_t phdr_vaddr{};
    uint16_t phdr_count{};
    uint16_t phdr_entry_size{};

    // TLS info
    uint64_t tls_image_addr{};
    uint64_t tls_image_size{};
    uint64_t tls_mem_size{};
    uint64_t tls_alignment{};

    std::vector<linux_exported_symbol> exports{};
    std::vector<std::string> needed_libraries{};
    std::vector<linux_mapped_section> sections{};

    // Library search paths extracted from ELF dynamic section
    std::string rpath{};   // DT_RPATH (legacy, searched before LD_LIBRARY_PATH)
    std::string runpath{}; // DT_RUNPATH (modern, searched after LD_LIBRARY_PATH)

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
};

// Map a statically-linked ELF binary from file data into emulated memory
linux_mapped_module map_elf_from_data(linux_memory_manager& memory, const std::span<const std::byte> data,
                                      const std::filesystem::path& path, uint64_t forced_base = 0);

// Apply relocations to a mapped ELF module
void apply_elf_relocations(linux_memory_manager& memory, linux_mapped_module& mod, const std::span<const std::byte> data,
                           int64_t base_delta);
