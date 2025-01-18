#pragma once
#include <span>
#include <string>
#include <cstddef>
#include <sstream>
#include <cwctype>
#include <algorithm>

namespace utils::string
{
    inline char char_to_lower(const char val)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(val)));
    }

    inline char16_t char_to_lower(const char16_t val)
    {
        if (val >= u'A' && val <= u'Z')
        {
            return val + 32;
        }

        return val;
    }

    inline wchar_t char_to_lower(const wchar_t val)
    {
        return std::towlower(val);
    }

    template <class Elem, class Traits, class Alloc>
    void to_lower_inplace(std::basic_string<Elem, Traits, Alloc>& str)
    {
        std::ranges::transform(str, str.begin(), [](const Elem e) { return char_to_lower(e); });
    }

    template <class Elem, class Traits, class Alloc>
    std::basic_string<Elem, Traits, Alloc> to_lower(std::basic_string<Elem, Traits, Alloc> str)
    {
        to_lower_inplace(str);
        return str;
    }

    template <class Elem, class Traits, class Alloc>
    std::basic_string<Elem, Traits, Alloc> to_lower_consume(std::basic_string<Elem, Traits, Alloc>& str)
    {
        return to_lower(std::move(str));
    }

    template <typename Integer>
        requires(std::is_integral_v<Integer>)
    std::string to_hex_string(const Integer& i)
    {
        std::stringstream stream{};
        stream << std::hex << i;
        return stream.str();
    }

    inline std::string to_hex_string(const void* data, const size_t size)
    {
        std::stringstream stream{};
        stream << std::hex;

        for (size_t i = 0; i < size; ++i)
        {
            const auto value = static_cast<const uint8_t*>(data)[i];
            stream << value;
        }

        return stream.str();
    }

    inline std::string to_hex_string(const std::span<std::byte> data)
    {
        return to_hex_string(data.data(), data.size());
    }

    inline uint8_t parse_nibble(const char nibble)
    {
        const auto lower = char_to_lower(nibble);

        if (lower >= '0' && lower <= '9')
        {
            return static_cast<uint8_t>(lower - '0');
        }

        if (lower >= 'a' && lower <= 'f')
        {
            return static_cast<uint8_t>(lower - 'a');
        }

        return 0;
    }

    inline std::vector<std::byte> from_hex_string(const std::string_view str)
    {
        const auto size = str.size() / 2;

        std::vector<std::byte> data{};
        data.resize(size);

        for (size_t i = 0; i < size; ++i)
        {
            const auto high = parse_nibble(str[i * 2 + 0]);
            const auto low = parse_nibble(str[i * 2 + 1]);
            const auto value = static_cast<std::byte>((high << 4) | low);

            data.push_back(value);
        }

        return data;
    }
}
