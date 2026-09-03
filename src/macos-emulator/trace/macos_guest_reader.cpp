#include "std_include.hpp"

#include "macos_guest_reader.hpp"

#include <array>
#include <cstdio>

namespace sogen
{
    namespace
    {
        constexpr std::array<char, 17> hex_digits{"0123456789abcdef"};

        void append_hex_byte(std::string& target, const uint8_t value)
        {
            target.push_back(hex_digits[value >> 4]);
            target.push_back(hex_digits[value & 0x0F]);
        }
    }

    std::optional<macos_guest_text> read_bounded_guest_string(const memory_interface& memory, const uint64_t address,
                                                              const size_t max_length)
    {
        if (address == 0 || max_length == 0)
        {
            return std::nullopt;
        }

        const auto limit = max_length > MACOS_TRACE_MAX_STRING ? MACOS_TRACE_MAX_STRING : max_length;

        macos_guest_text result{};
        result.text.reserve(limit < 64 ? limit : 64);

        for (size_t i = 0; i < limit; ++i)
        {
            char character{};
            if (!memory.try_read_memory(address + i, &character, sizeof(character)))
            {
                if (i == 0)
                {
                    return std::nullopt;
                }

                result.truncated = true;
                return result;
            }

            if (character == '\0')
            {
                return result;
            }

            result.text.push_back(character);
        }

        result.truncated = true;
        return result;
    }

    std::optional<std::vector<uint8_t>> read_bounded_guest_bytes(const memory_interface& memory, const uint64_t address,
                                                                 const size_t length)
    {
        if (address == 0 || length == 0)
        {
            return std::nullopt;
        }

        const auto limit = length > MACOS_TRACE_MAX_BUFFER ? MACOS_TRACE_MAX_BUFFER : length;

        std::vector<uint8_t> result{};
        result.reserve(limit);

        for (size_t i = 0; i < limit; ++i)
        {
            uint8_t byte{};
            if (!memory.try_read_memory(address + i, &byte, sizeof(byte)))
            {
                break;
            }

            result.push_back(byte);
        }

        if (result.empty())
        {
            return std::nullopt;
        }

        return result;
    }

    std::string escape_trace_text(const std::string_view text)
    {
        std::string result{};
        result.reserve(text.size());

        for (const auto character : text)
        {
            const auto value = static_cast<uint8_t>(character);

            switch (character)
            {
            case '\n':
                result += "\\n";
                continue;
            case '\r':
                result += "\\r";
                continue;
            case '\t':
                result += "\\t";
                continue;
            case '"':
                result += "\\\"";
                continue;
            case '\\':
                result += "\\\\";
                continue;
            default:
                break;
            }

            if (value < 0x20 || value >= 0x7F)
            {
                result += "\\x";
                append_hex_byte(result, value);
                continue;
            }

            result.push_back(character);
        }

        return result;
    }

    std::string quote_trace_text(const macos_guest_text& value)
    {
        std::string result{};
        result.push_back('"');
        result += escape_trace_text(value.text);
        result.push_back('"');

        if (value.truncated)
        {
            result += "...";
        }

        return result;
    }

    bool is_printable_trace_run(const std::span<const uint8_t> bytes)
    {
        for (const auto byte : bytes)
        {
            if (byte == '\n' || byte == '\r' || byte == '\t')
            {
                continue;
            }

            if (byte < 0x20 || byte >= 0x7F)
            {
                return false;
            }
        }

        return true;
    }

    std::string format_byte_preview(const std::span<const uint8_t> bytes, const size_t offset, const size_t length,
                                    const size_t total_length)
    {
        // Underflow-safe: offset + length would wrap on a hostile pair and let the loop run off the span.
        const auto size = bytes.size();
        if (offset > size || length > size - offset)
        {
            return "<unreadable>";
        }

        std::string result{};
        result.reserve(length * 3 + 24);

        for (size_t i = 0; i < length; ++i)
        {
            if (i != 0)
            {
                result.push_back(' ');
            }

            append_hex_byte(result, bytes[offset + i]);
        }

        if (total_length > length)
        {
            result += " ... (";
            result += std::to_string(total_length);
            result += " bytes)";
        }

        return result;
    }

    std::string format_hex(const uint64_t value)
    {
        std::array<char, 24> buffer{};
        const auto count = std::snprintf(buffer.data(), buffer.size(), "0x%llx", static_cast<unsigned long long>(value));
        if (count <= 0)
        {
            return "0x0";
        }

        return {buffer.data(), static_cast<size_t>(count)};
    }
}
