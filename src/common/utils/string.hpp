#pragma once
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
}
