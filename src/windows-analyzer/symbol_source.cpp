#include "symbol_source.hpp"

#include <module/mapped_module.hpp>

namespace sogen
{
    std::optional<uint32_t> symbol_address_to_rva(const mapped_module& mod, const uint32_t section_index, const uint32_t offset)
    {
        if (section_index == 0 || section_index > mod.pe_sections.size())
        {
            return std::nullopt;
        }

        const auto& section = mod.pe_sections[section_index - 1];
        const auto section_size = std::max(section.virtual_size, section.raw_size);
        if (section_size != 0 && offset > section_size)
        {
            return std::nullopt;
        }

        const auto rva = section.virtual_address + offset;
        if (rva >= mod.size_of_image)
        {
            return std::nullopt;
        }

        return rva;
    }

} // namespace sogen
