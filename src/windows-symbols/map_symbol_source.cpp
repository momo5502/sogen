#include "map_symbol_source.hpp"

#include <windows_emulator.hpp>
#include <utils/string.hpp>

#include <charconv>
#include <fstream>

namespace sogen::symbols
{
    namespace
    {
        using section_symbol_mapping = std::map<std::pair<uint32_t, uint32_t>, std::string>;

        std::optional<std::pair<std::string_view, std::string_view>> parse_symbol_fields(std::string_view line)
        {
            constexpr std::string_view whitespace = " \t\r\n\f\v";

            const auto address_end = line.find_first_of(whitespace);
            if (address_end == std::string_view::npos)
            {
                return std::nullopt;
            }

            const auto address = line.substr(0, address_end);

            line.remove_prefix(address_end);
            const auto name_begin = line.find_first_not_of(whitespace);
            if (name_begin == std::string_view::npos)
            {
                return std::nullopt;
            }

            line.remove_prefix(name_begin);
            const auto name = line.substr(0, line.find_first_of(whitespace));

            return std::pair{address, name};
        }

        std::optional<std::pair<uint32_t, uint32_t>> parse_section_offset(const std::string_view value)
        {
            const auto colon = value.find(':');
            if (colon == std::string_view::npos)
            {
                return std::nullopt;
            }

            uint32_t section{};
            uint32_t offset{};
            const auto section_text = value.substr(0, colon);
            const auto offset_text = value.substr(colon + 1);

            auto result = std::from_chars(section_text.data(), section_text.data() + section_text.size(), section, 16);
            if (result.ec != std::errc{} || result.ptr != section_text.data() + section_text.size())
            {
                return std::nullopt;
            }

            result = std::from_chars(offset_text.data(), offset_text.data() + offset_text.size(), offset, 16);
            if (result.ec != std::errc{} || result.ptr != offset_text.data() + offset_text.size())
            {
                return std::nullopt;
            }

            return std::pair{section, offset};
        }

        section_symbol_mapping parse_map(const std::filesystem::path& path)
        {
            std::ifstream stream{path};
            if (!stream)
            {
                throw std::runtime_error("Failed to open MSVC MAP file: " + path.string());
            }

            section_symbol_mapping symbols{};
            bool parsing_symbols = false;
            std::string line{};
            while (std::getline(stream, line))
            {
                const auto trimmed = utils::string::trim(line);
                if (trimmed.starts_with("Address") && trimmed.find("Publics by Value") != std::string::npos)
                {
                    parsing_symbols = true;
                    continue;
                }

                if (!parsing_symbols || trimmed.empty())
                {
                    continue;
                }

                const auto fields = parse_symbol_fields(trimmed);
                if (!fields)
                {
                    continue;
                }

                const auto section_offset = parse_section_offset(fields->first);
                if (!section_offset || section_offset->first == 0)
                {
                    continue;
                }

                symbols.try_emplace(*section_offset, fields->second);
            }

            if (symbols.empty())
            {
                throw std::runtime_error("No public symbols found in MSVC MAP file: " + path.string());
            }

            return symbols;
        }

        bool targets_module(const map_symbol_input& input, const windows_emulator& win_emu, const mapped_module& mod)
        {
            if (!input.module)
            {
                return win_emu.mod_manager.executable == &mod;
            }

            return utils::string::equals_ignore_case(*input.module, mod.name) ||
                   (!mod.path.empty() && utils::string::equals_ignore_case(*input.module, mod.path.filename().string()));
        }
    }

    map_symbol_source::map_symbol_source(windows_emulator& win_emu, std::vector<map_symbol_input> inputs)
        : win_emu_(&win_emu),
          inputs_(std::move(inputs))
    {
    }

    loaded_symbols map_symbol_source::load(const mapped_module& mod) const
    {
        loaded_symbols result{};
        for (const auto& input : this->inputs_)
        {
            if (!targets_module(input, *this->win_emu_, mod))
            {
                continue;
            }

            try
            {
                this->on_event(map_load_event{
                    .status = map_load_status::started,
                    .module = mod.name,
                    .path = input.path,
                    .error = {},
                });

                const auto symbols = parse_map(input.path);
                size_t merged_symbols = 0;
                for (const auto& [address, name] : symbols)
                {
                    if (const auto rva = symbol_address_to_rva(mod, address.first, address.second))
                    {
                        result.address_names.try_emplace(*rva, name);
                        ++merged_symbols;
                    }
                }

                if (merged_symbols == 0)
                {
                    throw std::runtime_error("MSVC MAP file contains no symbols that map into " + mod.name + ": " + input.path.string());
                }

                this->on_event(map_load_event{
                    .status = map_load_status::loaded,
                    .module = mod.name,
                    .path = input.path,
                    .error = {},
                });
            }
            catch (const std::exception& e)
            {
                this->on_event(map_load_event{
                    .status = map_load_status::failed,
                    .module = mod.name,
                    .path = input.path,
                    .error = std::string{e.what()},
                });
            }
        }

        return result;
    }

} // namespace sogen::symbols
