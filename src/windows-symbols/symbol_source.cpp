#include "symbol_source.hpp"

#include <platform/platform.hpp>
#include <module/mapped_module.hpp>

namespace sogen::symbols
{
    std::optional<uint32_t> symbol_address_to_rva(const mapped_module& mod, const uint32_t section_index, const uint32_t offset)
    {
        if (section_index == 0 || section_index > mod.pe_sections.size())
        {
            return std::nullopt;
        }

        const auto& section = mod.pe_sections[section_index - 1];
        const auto section_size = std::max(section.virtual_size, section.raw_size);
        if (offset >= section_size)
        {
            return std::nullopt;
        }

        const auto rva = static_cast<uint64_t>(section.virtual_address) + offset;
        if (rva >= mod.size_of_image)
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(rva);
    }

} // namespace sogen::symbols
