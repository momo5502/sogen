#include "../std_include.hpp"
#include "registry_file.hpp"

#include <charconv>

#include <utils/io.hpp>
#include <utils/string.hpp>

namespace sogen
{
    namespace
    {
        struct parsed_registry_value
        {
            std::string name{};
            uint32_t type{};
            std::vector<std::byte> data{};
        };

        struct parsed_registry_key
        {
            std::filesystem::path path{};
            size_t line{};
            std::vector<parsed_registry_value> values{};
        };

        std::u16string_view trim_left(std::u16string_view value)
        {
            while (!value.empty() && (value.front() == u' ' || value.front() == u'\t'))
            {
                value.remove_prefix(1);
            }

            return value;
        }

        std::u16string_view trim_right(std::u16string_view value)
        {
            while (!value.empty() && (value.back() == u' ' || value.back() == u'\t'))
            {
                value.remove_suffix(1);
            }

            return value;
        }

        std::u16string_view trim(std::u16string_view value)
        {
            return trim_right(trim_left(value));
        }

        std::runtime_error parse_error(const std::string_view source_name, const size_t line, const std::string_view message)
        {
            return std::runtime_error(std::string{source_name} + ":" + std::to_string(line) + ": " + std::string{message});
        }

        std::u16string decode_registry_text(const std::span<const std::byte> contents, const std::string_view source_name)
        {
            const auto byte = [&](const size_t index) { return std::to_integer<uint8_t>(contents[index]); };

            if (contents.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE)
            {
                if ((contents.size() - 2) % 2 != 0)
                {
                    throw std::runtime_error(std::string{source_name} + ": invalid UTF-16 registry file length");
                }

                std::u16string result{};
                result.reserve((contents.size() - 2) / 2);
                for (size_t i = 2; i + 1 < contents.size(); i += 2)
                {
                    result.push_back(static_cast<char16_t>(byte(i) | (static_cast<uint16_t>(byte(i + 1)) << 8)));
                }
                return result;
            }

            if (contents.size() >= 2 && byte(0) == 0xFE && byte(1) == 0xFF)
            {
                if ((contents.size() - 2) % 2 != 0)
                {
                    throw std::runtime_error(std::string{source_name} + ": invalid UTF-16 registry file length");
                }

                std::u16string result{};
                result.reserve((contents.size() - 2) / 2);
                for (size_t i = 2; i + 1 < contents.size(); i += 2)
                {
                    result.push_back(static_cast<char16_t>((static_cast<uint16_t>(byte(i)) << 8) | byte(i + 1)));
                }
                return result;
            }

            size_t offset = 0;
            if (contents.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF)
            {
                offset = 3;
            }

            if (offset == contents.size())
            {
                return {};
            }

            const auto* data = reinterpret_cast<const char*>(contents.data() + offset);
            return u8_to_u16(std::string_view{data, contents.size() - offset});
        }

        std::optional<size_t> find_assignment_separator(const std::u16string_view line)
        {
            bool quoted = false;
            bool escaped = false;

            for (size_t i = 0; i < line.size(); ++i)
            {
                const auto character = line[i];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }

                if (quoted && character == u'\\')
                {
                    escaped = true;
                    continue;
                }

                if (character == u'"')
                {
                    quoted = !quoted;
                    continue;
                }

                if (!quoted && character == u'=')
                {
                    return i;
                }
            }

            return std::nullopt;
        }

        std::vector<std::pair<size_t, std::u16string>> build_logical_lines(const std::u16string_view text)
        {
            std::vector<std::pair<size_t, std::u16string>> lines{};
            size_t line_number = 1;
            size_t start = 0;

            while (start <= text.size())
            {
                const auto end = text.find_first_of(u"\r\n", start);
                const auto line_end = end == std::u16string_view::npos ? text.size() : end;
                auto line = std::u16string{text.substr(start, line_end - start)};

                if (!lines.empty())
                {
                    auto previous = trim_right(lines.back().second);
                    const auto equals = find_assignment_separator(previous);
                    const auto value = equals ? trim_left(previous.substr(*equals + 1)) : std::u16string_view{};
                    const auto is_hex_continuation = previous.ends_with(u'\\') && utils::string::starts_with_ignore_case(value, u"hex"sv);
                    if (is_hex_continuation)
                    {
                        lines.back().second.resize(previous.size() - 1);
                        const auto continuation = trim_left(line);
                        lines.back().second.append(continuation.begin(), continuation.end());
                    }
                    else
                    {
                        lines.emplace_back(line_number, std::move(line));
                    }
                }
                else
                {
                    lines.emplace_back(line_number, std::move(line));
                }

                if (end == std::u16string_view::npos)
                {
                    break;
                }

                start = end + 1;
                if (text[end] == u'\r' && start < text.size() && text[start] == u'\n')
                {
                    ++start;
                }
                ++line_number;
            }

            return lines;
        }

        std::u16string parse_quoted_string(std::u16string_view& input, const std::string_view source_name, const size_t line)
        {
            input = trim_left(input);
            if (input.empty() || input.front() != u'"')
            {
                throw parse_error(source_name, line, "expected a quoted string");
            }

            input.remove_prefix(1);
            std::u16string result{};
            while (!input.empty())
            {
                const auto current = input.front();
                input.remove_prefix(1);

                if (current == u'"')
                {
                    return result;
                }

                if (current != u'\\')
                {
                    result.push_back(current);
                    continue;
                }

                if (input.empty())
                {
                    throw parse_error(source_name, line, "unterminated escape sequence");
                }

                const auto escaped = input.front();
                input.remove_prefix(1);
                switch (escaped)
                {
                case u'\\':
                case u'"':
                    result.push_back(escaped);
                    break;
                case u'n':
                    result.push_back(u'\n');
                    break;
                case u'r':
                    result.push_back(u'\r');
                    break;
                default:
                    result.push_back(u'\\');
                    result.push_back(escaped);
                    break;
                }
            }

            throw parse_error(source_name, line, "unterminated quoted string");
        }

        uint32_t parse_hex_number(const std::u16string_view value, const std::string_view source_name, const size_t line)
        {
            if (value.empty())
            {
                throw parse_error(source_name, line, "missing hexadecimal value");
            }

            const auto text = u16_to_u8(value);
            uint32_t result{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
            if (error == std::errc::result_out_of_range)
            {
                throw parse_error(source_name, line, "hexadecimal value is too large");
            }
            if (error != std::errc{} || end != text.data() + text.size())
            {
                throw parse_error(source_name, line, "invalid hexadecimal digit");
            }

            return result;
        }

        uint8_t parse_hex_byte(const std::u16string_view value, const std::string_view source_name, const size_t line)
        {
            if (value.size() != 2)
            {
                throw parse_error(source_name, line, "hex byte must contain exactly two digits");
            }

            return static_cast<uint8_t>(parse_hex_number(value, source_name, line));
        }

        std::vector<std::byte> encode_registry_string(const std::u16string_view value)
        {
            std::vector<std::byte> data{};
            data.reserve((value.size() + 1) * 2);
            for (const auto character : value)
            {
                data.push_back(static_cast<std::byte>(character & 0xFF));
                data.push_back(static_cast<std::byte>((character >> 8) & 0xFF));
            }
            data.push_back(std::byte{0});
            data.push_back(std::byte{0});
            return data;
        }

        parsed_registry_value parse_registry_value(std::u16string_view line_text, const std::string_view source_name, const size_t line)
        {
            parsed_registry_value result{};
            line_text = trim(line_text);

            if (line_text.front() == u'@')
            {
                line_text.remove_prefix(1);
            }
            else
            {
                auto name = parse_quoted_string(line_text, source_name, line);
                result.name = u16_to_u8(name);
            }

            line_text = trim_left(line_text);
            if (line_text.empty() || line_text.front() != u'=')
            {
                throw parse_error(source_name, line, "expected '=' after registry value name");
            }
            line_text = trim(line_text.substr(1));

            if (line_text == u"-")
            {
                throw parse_error(source_name, line, "deleting registry values is not supported");
            }

            if (!line_text.empty() && line_text.front() == u'"')
            {
                const auto string_value = parse_quoted_string(line_text, source_name, line);
                if (!trim(line_text).empty())
                {
                    throw parse_error(source_name, line, "unexpected characters after string value");
                }

                result.type = REG_SZ;
                result.data = encode_registry_string(string_value);
                return result;
            }

            if (utils::string::starts_with_ignore_case(line_text, u"dword:"sv))
            {
                const auto number = parse_hex_number(trim(line_text.substr(6)), source_name, line);
                result.type = REG_DWORD;
                result.data = {
                    static_cast<std::byte>(number & 0xFF),
                    static_cast<std::byte>((number >> 8) & 0xFF),
                    static_cast<std::byte>((number >> 16) & 0xFF),
                    static_cast<std::byte>((number >> 24) & 0xFF),
                };
                return result;
            }

            if (!utils::string::starts_with_ignore_case(line_text, u"hex"sv))
            {
                throw parse_error(source_name, line, "unsupported registry value syntax");
            }

            line_text.remove_prefix(3);
            result.type = REG_BINARY;
            if (!line_text.empty() && line_text.front() == u'(')
            {
                const auto close = line_text.find(u')');
                if (close == std::u16string_view::npos)
                {
                    throw parse_error(source_name, line, "unterminated registry type");
                }
                result.type = parse_hex_number(line_text.substr(1, close - 1), source_name, line);
                line_text.remove_prefix(close + 1);
            }

            line_text = trim_left(line_text);
            if (line_text.empty() || line_text.front() != u':')
            {
                throw parse_error(source_name, line, "expected ':' before hexadecimal data");
            }
            line_text = trim(line_text.substr(1));

            while (!line_text.empty())
            {
                const auto comma = line_text.find(u',');
                const auto token = trim(line_text.substr(0, comma));
                if (token.empty())
                {
                    throw parse_error(source_name, line, "empty hexadecimal byte");
                }
                result.data.push_back(static_cast<std::byte>(parse_hex_byte(token, source_name, line)));

                if (comma == std::u16string_view::npos)
                {
                    break;
                }
                line_text = trim(line_text.substr(comma + 1));
            }

            return result;
        }

        std::u16string to_nt_registry_path(std::u16string_view path, const std::string_view source_name, const size_t line)
        {
            path = trim(path);
            if (path.empty())
            {
                throw parse_error(source_name, line, "empty registry path");
            }

            std::u16string normalized{path};
            std::ranges::replace(normalized, u'/', u'\\');
            while (normalized.size() > 1 && normalized.back() == u'\\')
            {
                normalized.pop_back();
            }

            if (utils::string::starts_with_ignore_case(std::u16string_view{normalized}, u"\\Registry\\"sv))
            {
                return normalized;
            }

            const auto separator = normalized.find(u'\\');
            const auto root = std::u16string_view{normalized}.substr(0, separator);
            const auto suffix =
                separator == std::u16string::npos ? std::u16string_view{} : std::u16string_view{normalized}.substr(separator);

            std::u16string nt_root{};
            if (utils::string::equals_ignore_case(root, u"HKEY_LOCAL_MACHINE"sv) || utils::string::equals_ignore_case(root, u"HKLM"sv))
            {
                nt_root = u"\\Registry\\Machine";
            }
            else if (utils::string::equals_ignore_case(root, u"HKEY_CURRENT_USER"sv) ||
                     utils::string::equals_ignore_case(root, u"HKCU"sv) || utils::string::equals_ignore_case(root, u"HKEY_USERS"sv) ||
                     utils::string::equals_ignore_case(root, u"HKU"sv))
            {
                nt_root = u"\\Registry\\User";
            }
            else if (utils::string::equals_ignore_case(root, u"HKEY_CLASSES_ROOT"sv) || utils::string::equals_ignore_case(root, u"HKCR"sv))
            {
                nt_root = u"\\Registry\\Machine\\Software\\Classes";
            }
            else if (utils::string::equals_ignore_case(root, u"HKEY_CURRENT_CONFIG"sv) ||
                     utils::string::equals_ignore_case(root, u"HKCC"sv))
            {
                nt_root = u"\\Registry\\Machine\\System\\CurrentControlSet\\Hardware Profiles\\Current";
            }
            else
            {
                throw parse_error(source_name, line, "unsupported registry root");
            }

            nt_root.append(suffix.begin(), suffix.end());
            return nt_root;
        }

        std::filesystem::path parse_registry_key_path(const std::u16string_view path, const std::string_view source_name, const size_t line)
        {
            return std::filesystem::path{to_nt_registry_path(path, source_name, line)};
        }

        std::vector<parsed_registry_key> parse_registry_file(const std::span<const std::byte> contents, const std::string_view source_name)
        {
            const auto text = decode_registry_text(contents, source_name);
            const auto lines = build_logical_lines(text);

            bool header_seen = false;
            parsed_registry_key* current_key = nullptr;
            std::vector<parsed_registry_key> keys{};

            for (const auto& [line_number, storage] : lines)
            {
                auto line = trim(storage);
                if (line.empty() || line.front() == u';' || line.front() == u'#')
                {
                    continue;
                }

                if (!header_seen)
                {
                    if (!utils::string::equals_ignore_case(line, u"Windows Registry Editor Version 5.00"sv))
                    {
                        throw parse_error(source_name, line_number, "missing or unsupported .reg file header");
                    }
                    header_seen = true;
                    continue;
                }

                if (line.front() == u'[')
                {
                    if (line.back() != u']')
                    {
                        throw parse_error(source_name, line_number, "unterminated registry key section");
                    }

                    auto path = trim(line.substr(1, line.size() - 2));
                    if (!path.empty() && path.front() == u'-')
                    {
                        throw parse_error(source_name, line_number, "deleting registry keys is not supported");
                    }

                    keys.push_back({
                        .path = parse_registry_key_path(path, source_name, line_number),
                        .line = line_number,
                    });
                    current_key = &keys.back();
                    continue;
                }

                if (!current_key)
                {
                    throw parse_error(source_name, line_number, "registry value appears before a key section");
                }

                current_key->values.push_back(parse_registry_value(line, source_name, line_number));
            }

            if (!header_seen)
            {
                throw std::runtime_error(std::string{source_name} + ": missing or unsupported .reg file header");
            }

            return keys;
        }
    }

    void import_registry_file_contents(registry_manager& registry, const std::span<const std::byte> contents,
                                       const std::string_view source_name)
    {
        auto keys = parse_registry_file(contents, source_name);
        for (const auto& key : keys)
        {
            if (!registry.can_create_key(key.path))
            {
                throw parse_error(source_name, key.line, "registry path does not belong to a loaded hive");
            }
        }

        for (auto& key : keys)
        {
            auto registry_key = registry.create_key(key.path);
            if (!registry_key)
            {
                throw parse_error(source_name, key.line, "registry path does not belong to a loaded hive");
            }

            for (auto& value : key.values)
            {
                registry.set_value(*registry_key, std::move(value.name), value.type, value.data);
            }
        }
    }

    void import_registry_file(registry_manager& registry, const std::filesystem::path& file)
    {
        std::vector<std::byte> contents{};
        if (!utils::io::read_file(file, &contents))
        {
            throw std::runtime_error("Failed to read registry file: " + file.string());
        }

        import_registry_file_contents(registry, contents, file.string());
    }
}
