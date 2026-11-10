#include "std_include.hpp"
#include "code_page.hpp"

#include "windows_emulator.hpp"
#include "registry/registry_utils.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <utils/serialization.hpp>

namespace sogen
{
    namespace
    {
        struct code_page_layout
        {
            uint16_t id{};
            uint16_t maximum_character_size{};
            size_t multibyte_offset{};
            size_t unicode_offset{};
            size_t dbcs_offset{};
        };

        uint16_t read_word(const std::span<const std::byte> data, const size_t offset)
        {
            return static_cast<uint16_t>(std::to_integer<uint16_t>(data[offset]) | (std::to_integer<uint16_t>(data[offset + 1]) << 8));
        }

        std::optional<code_page_layout> read_code_page_layout(const std::span<const std::byte> data)
        {
            constexpr size_t header_size = 13 * sizeof(uint16_t);
            constexpr size_t byte_table_size = 256 * sizeof(uint16_t);
            if (data.size() < header_size)
            {
                return std::nullopt;
            }

            code_page_layout result{};
            result.id = read_word(data, 2);
            result.maximum_character_size = read_word(data, 4);
            if (result.id == 0 || result.id == 65001 || (result.maximum_character_size != 1 && result.maximum_character_size != 2) ||
                (result.id == 1252 && result.maximum_character_size != 1))
            {
                return std::nullopt;
            }

            const auto table_offset = static_cast<size_t>(read_word(data, 0)) * sizeof(uint16_t);
            if (table_offset < header_size || table_offset > data.size() - sizeof(uint16_t))
            {
                return std::nullopt;
            }

            const auto unicode_word_offset = table_offset / sizeof(uint16_t) + read_word(data, table_offset);
            if (unicode_word_offset > std::numeric_limits<uint16_t>::max())
            {
                return std::nullopt;
            }
            result.multibyte_offset = table_offset + sizeof(uint16_t);
            result.unicode_offset = (unicode_word_offset + 1) * sizeof(uint16_t);
            const auto reverse_size = size_t{0x10000} * result.maximum_character_size;
            if (result.unicode_offset < result.multibyte_offset + byte_table_size + 2 * sizeof(uint16_t) ||
                result.unicode_offset > data.size() || data.size() - result.unicode_offset < reverse_size)
            {
                return std::nullopt;
            }

            const auto glyph_offset = result.multibyte_offset + byte_table_size;
            const auto ranges_offset = glyph_offset + sizeof(uint16_t) + (read_word(data, glyph_offset) != 0 ? byte_table_size : 0);
            if (ranges_offset > result.unicode_offset - sizeof(uint16_t))
            {
                return std::nullopt;
            }

            const bool dbcs = read_word(data, ranges_offset) != 0;
            if (dbcs != (result.maximum_character_size == 2))
            {
                return std::nullopt;
            }
            if (!dbcs)
            {
                return result;
            }

            result.dbcs_offset = ranges_offset + sizeof(uint16_t);
            if (result.unicode_offset - result.dbcs_offset < byte_table_size)
            {
                return std::nullopt;
            }

            std::array<bool, 256> lead_bytes{};
            uint8_t previous_end{};
            bool terminated{};
            for (size_t i = 14; i < header_size; i += 2)
            {
                const auto first = std::to_integer<uint8_t>(data[i]);
                const auto last = std::to_integer<uint8_t>(data[i + 1]);
                if (first == 0 && last == 0)
                {
                    terminated = true;
                    continue;
                }
                if (terminated || first == 0 || first > last || first <= previous_end)
                {
                    return std::nullopt;
                }
                for (size_t byte = first; byte <= last; ++byte)
                {
                    lead_bytes[byte] = true;
                }
                previous_end = last;
            }

            const auto dbcs_words = (result.unicode_offset - result.dbcs_offset) / sizeof(uint16_t);
            for (size_t i = 0; i < lead_bytes.size(); ++i)
            {
                const auto offset = read_word(data, result.dbcs_offset + i * sizeof(uint16_t));
                if ((offset != 0) != lead_bytes[i] || (offset != 0 && (offset < 256 || offset > dbcs_words - 256)))
                {
                    return std::nullopt;
                }
            }
            return result;
        }

        struct single_byte_code_page
        {
            std::array<uint16_t, 256> to_unicode{};
            std::span<const std::byte> from_unicode{};
        };

        std::optional<single_byte_code_page> read_single_byte_code_page(const std::span<const std::byte> data)
        {
            const auto layout = read_code_page_layout(data);
            if (!layout || layout->maximum_character_size != 1)
            {
                return std::nullopt;
            }

            single_byte_code_page result{};
            for (size_t i = 0; i < result.to_unicode.size(); ++i)
            {
                result.to_unicode[i] = read_word(data, layout->multibyte_offset + i * sizeof(uint16_t));
            }
            result.from_unicode = data.subspan(layout->unicode_offset, 0x10000);
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

        code_page load_code_page(windows_emulator& win_emu, const uint32_t id)
        {
            const auto path = win_emu.version.get_system_root() / "System32" / ("c_" + std::to_string(id) + ".nls");
            auto data = win_emu.file_sys.read_file(path);
            if (data.empty())
            {
                throw std::runtime_error("Missing NLS file for code page " + std::to_string(id));
            }
            code_page result{std::move(data)};
            if (result.id() != id)
            {
                throw std::runtime_error("NLS code page does not match requested code page " + std::to_string(id));
            }
            return result;
        }
    }

    code_page::code_page(std::vector<std::byte> data)
        : data_(std::move(data))
    {
        const auto layout = read_code_page_layout(this->data_);
        if (!layout)
        {
            throw std::invalid_argument("Missing, invalid or unsupported NLS code page");
        }
        this->id_ = layout->id;
        this->maximum_character_size_ = layout->maximum_character_size;
        this->multibyte_offset_ = layout->multibyte_offset;
        this->unicode_offset_ = layout->unicode_offset;
        this->dbcs_offset_ = layout->dbcs_offset;
    }

    uint16_t code_page::id() const
    {
        return this->id_;
    }

    uint16_t code_page::maximum_character_size() const
    {
        return this->maximum_character_size_;
    }

    std::span<const std::byte> code_page::data() const
    {
        return this->data_;
    }

    std::u16string code_page::decode(const std::string_view text) const
    {
        if (this->id_ == 1252)
        {
            return cp1252_to_u16(text);
        }

        std::u16string result{};
        result.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            const auto byte = static_cast<uint8_t>(text[i]);
            const auto offset = this->dbcs_offset_ == 0 ? uint16_t{} : read_word(this->data_, this->dbcs_offset_ + byte * sizeof(uint16_t));
            if (offset == 0)
            {
                result.push_back(static_cast<char16_t>(read_word(this->data_, this->multibyte_offset_ + byte * sizeof(uint16_t))));
            }
            else if (i + 1 < text.size())
            {
                const auto trail = static_cast<uint8_t>(text[++i]);
                result.push_back(static_cast<char16_t>(read_word(this->data_, this->dbcs_offset_ + (offset + trail) * sizeof(uint16_t))));
            }
            else
            {
                result.push_back(u'\0');
            }
        }
        return result;
    }

    uint16_t code_page::encode_character(const char16_t character) const
    {
        if (this->id_ == 1252)
        {
            return static_cast<uint8_t>(u16_to_cp1252_char(character));
        }
        if (this->maximum_character_size_ == 1)
        {
            return std::to_integer<uint8_t>(this->data_[this->unicode_offset_ + character]);
        }
        return read_word(this->data_, this->unicode_offset_ + static_cast<size_t>(character) * sizeof(uint16_t));
    }

    std::string code_page::encode(const std::u16string_view text, const size_t maximum_bytes) const
    {
        std::string result{};
        result.reserve(std::min(text.size(), maximum_bytes));
        for (const auto character : text)
        {
            const auto encoded = this->encode_character(character);
            const size_t size = encoded > 0xFF ? 2 : 1;
            if (size > maximum_bytes - result.size())
            {
                break;
            }
            if (size == 2)
            {
                result.push_back(static_cast<char>(encoded >> 8));
            }
            result.push_back(static_cast<char>(encoded & 0xFF));
        }
        return result;
    }

    size_t code_page::encoded_size(const std::u16string_view text) const
    {
        if (this->maximum_character_size_ == 1)
        {
            return text.size();
        }
        size_t result{};
        for (const auto character : text)
        {
            const size_t size = this->encode_character(character) > 0xFF ? 2 : 1;
            if (result > std::numeric_limits<size_t>::max() - size)
            {
                throw std::length_error("Encoded text is too large");
            }
            result += size;
        }
        return result;
    }

    void code_page::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_vector(this->data_);
    }

    void code_page::deserialize(utils::buffer_deserializer& buffer)
    {
        std::vector<std::byte> data{};
        buffer.read_vector(data);
        *this = data.empty() ? code_page{} : code_page{std::move(data)};
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

    uint64_t map_nls_data(windows_emulator& win_emu, const std::span<const std::byte> data)
    {
        if (data.empty() || data.size() > std::numeric_limits<size_t>::max() - 0xFFF)
        {
            return 0;
        }
        const auto size = static_cast<size_t>(page_align_up(data.size()));
        const auto start = win_emu.process.is_wow64_process ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
        const auto highest_address = win_emu.process.is_wow64_process ? uint64_t{0xFFFFFFFF} : MAX_ALLOCATION_ADDRESS;
        const auto address = win_emu.memory.allocate_memory(size, memory_permission::read, false, start,
                                                            memory_region_kind::private_allocation, highest_address);
        if (address != 0)
        {
            win_emu.emu().write_memory(address, data.data(), data.size());
        }
        return address;
    }

    void configure_process_code_pages(registry_manager& registry, const uint32_t ansi_id, const uint32_t oem_id)
    {
        for (const auto id : {ansi_id, oem_id})
        {
            if (id <= 3 || id > std::numeric_limits<uint16_t>::max())
            {
                throw std::invalid_argument("Invalid code-page identifier " + std::to_string(id));
            }
            if (id == 65000 || id == 65001)
            {
                throw std::invalid_argument("Unsupported ANSI/OEM code page " + std::to_string(id));
            }
        }

        const auto key = registry.create_key(R"(\Registry\Machine\System\CurrentControlSet\Control\Nls\CodePage)");
        if (!key)
        {
            throw std::runtime_error("Cannot configure ANSI/OEM code pages without the SYSTEM registry hive");
        }
        const auto write_code_page = [&](const std::string_view name, const uint32_t id) {
            const auto text = u8_to_u16(std::to_string(id));
            registry.set_value(*key, std::string{name}, REG_SZ, std::as_bytes(std::span{text.c_str(), text.size() + 1}));
        };
        write_code_page("ACP", ansi_id);
        write_code_page("OEMCP", oem_id);
    }

    void initialize_process_code_pages(windows_emulator& win_emu)
    {
        const auto ansi_id = get_code_page(win_emu.registry, "ACP", 1252);
        const auto oem_id = get_code_page(win_emu.registry, "OEMCP", 437);
        if (!ansi_id || !oem_id)
        {
            throw std::runtime_error("Invalid ANSI/OEM code-page configuration");
        }
        auto ansi = load_code_page(win_emu, *ansi_id);
        auto oem = *oem_id == *ansi_id ? ansi : load_code_page(win_emu, *oem_id);
        win_emu.process.ansi_code_page = std::move(ansi);
        win_emu.process.oem_code_page = std::move(oem);
    }

    void initialize_user_code_page_tables(windows_emulator& win_emu)
    {
        const auto& ansi = win_emu.process.ansi_code_page;
        const auto& oem = win_emu.process.oem_code_page;
        if (!oem)
        {
            return;
        }
        std::optional<ansi_oem_tables> tables{};
        if (ansi.maximum_character_size() == 2 || oem->maximum_character_size() == 2)
        {
            if (ansi.id() != oem->id())
            {
                throw std::runtime_error("USER byte tables do not support different ANSI/OEM multibyte code pages");
            }
            tables.emplace();
            for (size_t i = 0; i < tables->ansi_to_oem.size(); ++i)
            {
                tables->ansi_to_oem[i] = static_cast<uint8_t>(i);
                tables->oem_to_ansi[i] = static_cast<uint8_t>(i);
            }
        }
        else
        {
            tables = build_ansi_oem_tables(ansi.data(), oem->data());
        }
        if (!tables)
        {
            throw std::runtime_error("Cannot initialize ANSI/OEM character tables");
        }
        win_emu.process.user_handles.get_server_info().access([&](USER_SERVERINFO& info) {
            std::ranges::copy(tables->ansi_to_oem, info.ansiToOem);
            std::ranges::copy(tables->oem_to_ansi, info.oemToAnsi);
        });
    }
}
