#include "std_include.hpp"
#include "code_page.hpp"

#include "windows_emulator.hpp"
#include "registry/registry_utils.hpp"

#include <algorithm>
#include <charconv>
#include <utils/io.hpp>

namespace sogen
{
    namespace
    {
        struct single_byte_code_page
        {
            std::array<uint16_t, 256> to_unicode{};
            std::span<const std::byte> from_unicode{};
        };

        uint16_t read_word(const std::span<const std::byte> data, const size_t offset)
        {
            return static_cast<uint16_t>(std::to_integer<uint16_t>(data[offset]) | (std::to_integer<uint16_t>(data[offset + 1]) << 8));
        }

        std::optional<single_byte_code_page> read_single_byte_code_page(const std::span<const std::byte> data)
        {
            constexpr size_t header_size = 13 * sizeof(uint16_t);
            constexpr size_t unicode_table_size = 0x10000;
            constexpr size_t maximum_character_size_offset = 2 * sizeof(uint16_t);
            constexpr uint16_t single_byte_character_size = 1;
            if (data.size() < header_size || read_word(data, maximum_character_size_offset) != single_byte_character_size)
            {
                return std::nullopt;
            }

            const auto table_offset = static_cast<size_t>(read_word(data, 0)) * sizeof(uint16_t);
            if (table_offset < header_size || table_offset > data.size() - sizeof(uint16_t))
            {
                return std::nullopt;
            }

            // NLS offsets are in WORDs; the reverse SBCS table contains one byte per UTF-16 code unit.
            const auto reverse_offset = table_offset + (static_cast<size_t>(read_word(data, table_offset)) + 1) * sizeof(uint16_t);
            const auto forward_offset = table_offset + sizeof(uint16_t);
            if (data.size() - forward_offset < 256 * sizeof(uint16_t) || reverse_offset < forward_offset + 258 * sizeof(uint16_t) ||
                reverse_offset > data.size() || data.size() - reverse_offset < unicode_table_size)
            {
                return std::nullopt;
            }

            single_byte_code_page result{};
            for (size_t i = 0; i < result.to_unicode.size(); ++i)
            {
                result.to_unicode[i] = read_word(data, forward_offset + i * sizeof(uint16_t));
            }
            result.from_unicode = data.subspan(reverse_offset, unicode_table_size);
            return result;
        }

        std::optional<uint32_t> get_code_page(registry_manager& registry, const std::string_view name, const uint32_t fallback)
        {
            const auto value =
                registry_utils::read_registry_string(registry, R"(\Registry\Machine\System\CurrentControlSet\Control\Nls\CodePage)", name);
            if (!value)
            {
                return fallback;
            }
            const auto text = u16_to_u8(*value);
            uint32_t result{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
            if (error != std::errc{} || end != text.data() + text.size())
            {
                return std::nullopt;
            }
            return result;
        }
    }

    std::optional<ansi_oem_tables> build_ansi_oem_tables(const std::span<const std::byte> ansi, const std::span<const std::byte> oem)
    {
        const auto ansi_page = read_single_byte_code_page(ansi);
        const auto oem_page = read_single_byte_code_page(oem);
        if (!ansi_page || !oem_page)
        {
            return std::nullopt;
        }

        ansi_oem_tables result{};
        for (size_t i = 0; i < result.ansi_to_oem.size(); ++i)
        {
            result.ansi_to_oem[i] = std::to_integer<uint8_t>(oem_page->from_unicode[ansi_page->to_unicode[i]]);
            result.oem_to_ansi[i] = std::to_integer<uint8_t>(ansi_page->from_unicode[oem_page->to_unicode[i]]);
        }
        return result;
    }

    void initialize_user_code_page_tables(windows_emulator& win_emu)
    {
        const auto ansi_page = get_code_page(win_emu.registry, "ACP", 1252);
        const auto oem_page = get_code_page(win_emu.registry, "OEMCP", 437);
        if (!ansi_page || !oem_page)
        {
            win_emu.log.warn("Cannot initialize ANSI/OEM character tables: invalid code-page configuration\n");
            return;
        }

        const auto directory = win_emu.version.get_system_root() / "System32";
        const auto ansi = win_emu.file_sys.read_file(directory / ("c_" + std::to_string(*ansi_page) + ".nls"));
        const auto oem = win_emu.file_sys.read_file(directory / ("c_" + std::to_string(*oem_page) + ".nls"));
        const auto tables = build_ansi_oem_tables(ansi, oem);
        if (!tables)
        {
            win_emu.log.warn(
                "Cannot initialize ANSI/OEM character tables for code pages %u/%u: missing, invalid or unsupported NLS files\n", *ansi_page,
                *oem_page);
            return;
        }

        win_emu.process.user_handles.get_server_info().access([&](USER_SERVERINFO& info) {
            std::ranges::copy(tables->ansi_to_oem, info.ansiToOem);
            std::ranges::copy(tables->oem_to_ansi, info.oemToAnsi);
        });
    }
}
