#include "plist.hpp"

#include <cstdint>

namespace sogen
{
    namespace
    {
        constexpr std::string_view BINARY_MAGIC = "bplist00";
        constexpr size_t BINARY_TRAILER_SIZE = 32;

        void append_utf8(std::string& out, const uint32_t code_point)
        {
            if (code_point < 0x80)
            {
                out.push_back(static_cast<char>(code_point));
            }
            else if (code_point < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
            else if (code_point < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
            }
        }

        std::string_view as_text(const std::span<const std::byte> data)
        {
            return std::string_view{reinterpret_cast<const char*>(data.data()), data.size()};
        }

        std::optional<std::string> decode_entities(const std::string_view raw)
        {
            std::string out{};
            out.reserve(raw.size());

            size_t index = 0;
            while (index < raw.size())
            {
                if (raw[index] != '&')
                {
                    out.push_back(raw[index]);
                    ++index;
                    continue;
                }

                const auto end = raw.find(';', index);
                if (end == std::string_view::npos || end - index > 12)
                {
                    return std::nullopt;
                }

                const auto entity = raw.substr(index + 1, end - index - 1);
                if (entity == "amp")
                {
                    out.push_back('&');
                }
                else if (entity == "lt")
                {
                    out.push_back('<');
                }
                else if (entity == "gt")
                {
                    out.push_back('>');
                }
                else if (entity == "quot")
                {
                    out.push_back('"');
                }
                else if (entity == "apos")
                {
                    out.push_back('\'');
                }
                else if (entity.size() > 1 && entity[0] == '#')
                {
                    const auto is_hex = entity[1] == 'x' || entity[1] == 'X';
                    const auto digits = entity.substr(is_hex ? 2 : 1);
                    if (digits.empty() || digits.size() > 8)
                    {
                        return std::nullopt;
                    }

                    uint32_t value = 0;
                    for (const char ch : digits)
                    {
                        uint32_t digit = 0;
                        if (ch >= '0' && ch <= '9')
                        {
                            digit = static_cast<uint32_t>(ch - '0');
                        }
                        else if (is_hex && ch >= 'a' && ch <= 'f')
                        {
                            digit = static_cast<uint32_t>(ch - 'a') + 10;
                        }
                        else if (is_hex && ch >= 'A' && ch <= 'F')
                        {
                            digit = static_cast<uint32_t>(ch - 'A') + 10;
                        }
                        else
                        {
                            return std::nullopt;
                        }

                        value = value * (is_hex ? 16u : 10u) + digit;
                    }

                    if (value > 0x10FFFF)
                    {
                        return std::nullopt;
                    }

                    append_utf8(out, value);
                }
                else
                {
                    return std::nullopt;
                }

                index = end + 1;
            }

            return out;
        }

        struct xml_tag
        {
            std::string_view name{};
            size_t content_start{};
            bool closing{};
            bool self_closing{};
        };

        std::optional<xml_tag> next_tag(const std::string_view text, size_t& cursor)
        {
            while (cursor < text.size())
            {
                const auto open = text.find('<', cursor);
                if (open == std::string_view::npos)
                {
                    return std::nullopt;
                }

                if (text.compare(open, 4, "<!--") == 0)
                {
                    const auto end = text.find("-->", open);
                    if (end == std::string_view::npos)
                    {
                        return std::nullopt;
                    }
                    cursor = end + 3;
                    continue;
                }

                if (open + 1 < text.size() && (text[open + 1] == '?' || text[open + 1] == '!'))
                {
                    const auto end = text.find('>', open);
                    if (end == std::string_view::npos)
                    {
                        return std::nullopt;
                    }
                    cursor = end + 1;
                    continue;
                }

                const auto end = text.find('>', open);
                if (end == std::string_view::npos)
                {
                    return std::nullopt;
                }

                auto body = text.substr(open + 1, end - open - 1);
                xml_tag tag{};
                if (!body.empty() && body.front() == '/')
                {
                    tag.closing = true;
                    body.remove_prefix(1);
                }
                if (!body.empty() && body.back() == '/')
                {
                    tag.self_closing = true;
                    body.remove_suffix(1);
                }

                const auto name_end = body.find_first_of(" \t\r\n");
                tag.name = name_end == std::string_view::npos ? body : body.substr(0, name_end);
                tag.content_start = end + 1;

                cursor = end + 1;
                return tag;
            }

            return std::nullopt;
        }

        std::optional<std::string> read_element_text(const std::string_view text, const std::string_view name, size_t& cursor)
        {
            const auto start = cursor;
            const std::string closing = "</" + std::string{name} + ">";
            const auto end = text.find(closing, start);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }

            cursor = end + closing.size();
            return decode_entities(text.substr(start, end - start));
        }

        std::optional<std::string> xml_top_level_string(const std::string_view text, const std::string_view key)
        {
            size_t cursor = 0;
            size_t depth = 0;
            bool key_matched = false;

            while (true)
            {
                const auto tag = next_tag(text, cursor);
                if (!tag)
                {
                    return std::nullopt;
                }

                if (tag->name == "dict")
                {
                    if (tag->self_closing)
                    {
                        continue;
                    }
                    if (tag->closing)
                    {
                        if (depth == 0)
                        {
                            return std::nullopt;
                        }
                        --depth;
                    }
                    else
                    {
                        ++depth;
                    }
                    continue;
                }

                if (tag->closing || depth != 1)
                {
                    continue;
                }

                if (key_matched)
                {
                    if (tag->name != "string")
                    {
                        return std::nullopt;
                    }
                    if (tag->self_closing)
                    {
                        return std::string{};
                    }
                    return read_element_text(text, "string", cursor);
                }

                if (tag->name == "key" && !tag->self_closing)
                {
                    const auto name = read_element_text(text, "key", cursor);
                    if (!name)
                    {
                        return std::nullopt;
                    }
                    key_matched = *name == key;
                }
            }
        }

        uint64_t read_big_endian(const std::span<const std::byte> data, const size_t offset, const size_t size)
        {
            uint64_t value = 0;
            for (size_t i = 0; i < size; ++i)
            {
                value = (value << 8) | static_cast<uint64_t>(data[offset + i]);
            }
            return value;
        }

        struct binary_object
        {
            uint8_t type{};
            uint64_t count{};
            size_t payload{};
        };

        std::optional<binary_object> read_object_head(const std::span<const std::byte> data, const size_t offset)
        {
            if (offset >= data.size())
            {
                return std::nullopt;
            }

            const auto marker = static_cast<uint8_t>(data[offset]);
            binary_object object{};
            object.type = static_cast<uint8_t>(marker >> 4);
            object.count = marker & 0x0FU;
            object.payload = offset + 1;

            if (object.count != 0x0F || object.type == 0)
            {
                return object;
            }

            if (object.payload >= data.size())
            {
                return std::nullopt;
            }

            const auto size_marker = static_cast<uint8_t>(data[object.payload]);
            if ((size_marker >> 4) != 0x1 || (size_marker & 0x0FU) > 3)
            {
                return std::nullopt;
            }

            const size_t width = size_t{1} << (size_marker & 0x0FU);
            const auto start = object.payload + 1;
            if (start > data.size() || width > data.size() - start)
            {
                return std::nullopt;
            }

            object.count = read_big_endian(data, start, width);
            object.payload = start + width;
            return object;
        }

        std::optional<std::string> read_binary_string(const std::span<const std::byte> data, const size_t offset)
        {
            const auto object = read_object_head(data, offset);
            if (!object)
            {
                return std::nullopt;
            }

            if (object->type == 0x5)
            {
                if (object->payload > data.size() || object->count > data.size() - object->payload)
                {
                    return std::nullopt;
                }

                std::string value{};
                value.reserve(static_cast<size_t>(object->count));
                for (uint64_t i = 0; i < object->count; ++i)
                {
                    value.push_back(static_cast<char>(data[object->payload + static_cast<size_t>(i)]));
                }
                return value;
            }

            if (object->type != 0x6)
            {
                return std::nullopt;
            }

            if (object->count > data.size() / 2)
            {
                return std::nullopt;
            }

            const auto byte_count = static_cast<size_t>(object->count) * 2;
            if (object->payload > data.size() || byte_count > data.size() - object->payload)
            {
                return std::nullopt;
            }

            std::string value{};
            for (uint64_t i = 0; i < object->count; ++i)
            {
                const auto unit = static_cast<uint32_t>(read_big_endian(data, object->payload + static_cast<size_t>(i) * 2, 2));
                if (unit >= 0xD800 && unit < 0xDC00)
                {
                    if (i + 1 >= object->count)
                    {
                        return std::nullopt;
                    }
                    const auto low = static_cast<uint32_t>(read_big_endian(data, object->payload + static_cast<size_t>(i + 1) * 2, 2));
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        return std::nullopt;
                    }
                    append_utf8(value, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    ++i;
                    continue;
                }

                if (unit >= 0xDC00 && unit <= 0xDFFF)
                {
                    return std::nullopt;
                }

                append_utf8(value, unit);
            }

            return value;
        }

        std::optional<std::string> binary_top_level_string(const std::span<const std::byte> data, const std::string_view key)
        {
            if (data.size() < BINARY_MAGIC.size() + BINARY_TRAILER_SIZE)
            {
                return std::nullopt;
            }

            const auto trailer = data.size() - BINARY_TRAILER_SIZE;
            const size_t offset_size = static_cast<uint8_t>(data[trailer + 6]);
            const size_t ref_size = static_cast<uint8_t>(data[trailer + 7]);
            const auto object_count = read_big_endian(data, trailer + 8, 8);
            const auto top_object = read_big_endian(data, trailer + 16, 8);
            const auto table_offset = read_big_endian(data, trailer + 24, 8);

            if (offset_size < 1 || offset_size > 8 || ref_size < 1 || ref_size > 8)
            {
                return std::nullopt;
            }

            if (object_count == 0 || object_count > data.size() || top_object >= object_count)
            {
                return std::nullopt;
            }

            const auto table_bytes = static_cast<size_t>(object_count) * offset_size;
            if (table_offset < BINARY_MAGIC.size() || table_offset > data.size() || table_bytes > data.size() - table_offset)
            {
                return std::nullopt;
            }

            const auto offset_of = [&](const uint64_t index) -> std::optional<size_t> {
                const auto value =
                    read_big_endian(data, static_cast<size_t>(table_offset) + static_cast<size_t>(index) * offset_size, offset_size);
                if (value >= data.size())
                {
                    return std::nullopt;
                }
                return static_cast<size_t>(value);
            };

            const auto root_offset = offset_of(top_object);
            if (!root_offset)
            {
                return std::nullopt;
            }

            const auto root = read_object_head(data, *root_offset);
            if (!root || root->type != 0xD)
            {
                return std::nullopt;
            }

            if (root->count > object_count)
            {
                return std::nullopt;
            }

            const auto entries = static_cast<size_t>(root->count);
            const auto refs_bytes = entries * ref_size;
            if (root->payload > data.size() || refs_bytes > (data.size() - root->payload) / 2)
            {
                return std::nullopt;
            }

            for (size_t i = 0; i < entries; ++i)
            {
                const auto key_ref = read_big_endian(data, root->payload + i * ref_size, ref_size);
                if (key_ref >= object_count)
                {
                    return std::nullopt;
                }

                const auto key_offset = offset_of(key_ref);
                if (!key_offset)
                {
                    return std::nullopt;
                }

                const auto name = read_binary_string(data, *key_offset);
                if (!name || *name != key)
                {
                    continue;
                }

                const auto value_ref = read_big_endian(data, root->payload + refs_bytes + i * ref_size, ref_size);
                if (value_ref >= object_count)
                {
                    return std::nullopt;
                }

                const auto value_offset = offset_of(value_ref);
                if (!value_offset)
                {
                    return std::nullopt;
                }

                return read_binary_string(data, *value_offset);
            }

            return std::nullopt;
        }
    }

    std::optional<std::string> plist_top_level_string(const std::span<const std::byte> data, const std::string_view key)
    {
        if (data.empty() || data.size() > MAX_PLIST_SIZE || key.empty())
        {
            return std::nullopt;
        }

        const auto text = as_text(data);
        if (text.starts_with(BINARY_MAGIC))
        {
            return binary_top_level_string(data, key);
        }

        if (text.find('<') == std::string_view::npos)
        {
            return std::nullopt;
        }

        return xml_top_level_string(text, key);
    }
}
