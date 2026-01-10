#pragma once
#include <span>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <algorithm>
#include <string_view>

namespace utils::string
{
#ifdef __clang__
    __attribute__((__format__(__printf__, 1, 2)))
#endif
    const char* va(const char* format, ...);

    template <typename T, size_t Size>
        requires(std::is_trivially_copyable_v<T>)
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
    void copy(T (&array)[Size], const T* str)
    {
        if constexpr (Size == 0)
        {
            return;
        }

        const auto size = std::min(Size, std::char_traits<T>::length(str));
        memcpy(array, str, size * sizeof(T));
        array[std::min(Size - 1, size)] = {};
    }

    inline char char_to_lower(const char val)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(val)));
    }

    inline wchar_t char_to_lower(const wchar_t val)
    {
        return static_cast<wchar_t>(std::towlower(val));
    }

    inline char16_t char_to_lower(const char16_t val)
    {
        static_assert(sizeof(char16_t) <= sizeof(wchar_t));
        static_assert(sizeof(char16_t) == sizeof(uint16_t));
        return static_cast<char16_t>(char_to_lower(static_cast<wchar_t>(static_cast<uint16_t>(val))));
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

    inline char to_nibble(std::byte value, const bool uppercase = false)
    {
        value = value & static_cast<std::byte>(0xF);

        if (value <= static_cast<std::byte>(9))
        {
            return static_cast<char>('0' + static_cast<char>(value));
        }

        return static_cast<char>((uppercase ? 'A' : 'a') + (static_cast<char>(value) - 0xA));
    }

    inline std::pair<char, char> to_hex(const std::byte value, const bool uppercase = false)
    {
        return {to_nibble(value >> 4, uppercase), to_nibble(value, uppercase)};
    }

    inline std::string to_hex_string(const void* data, const size_t size, const bool uppercase = false)
    {
        std::string result{};
        result.reserve(size * 2);

        for (size_t i = 0; i < size; ++i)
        {
            const auto value = static_cast<const std::byte*>(data)[i];
            const auto [high, low] = to_hex(value, uppercase);
            result.push_back(high);
            result.push_back(low);
        }

        return result;
    }

    template <typename Integer>
        requires(std::is_integral_v<Integer>)
    std::string to_hex_number(const Integer& i, const bool uppercase = false)
    {
        std::string res{};
        res.reserve(sizeof(i) * 2);

        const std::span data{reinterpret_cast<const std::byte*>(&i), sizeof(i)};

        for (const auto value : data)
        {
            const auto [high, low] = to_hex(value, uppercase);
            res.insert(res.begin(), {high, low});
        }

        while (res.size() > 1 && res.front() == '0')
        {
            res.erase(res.begin());
        }

        return res;
    }

    template <typename Integer>
        requires(std::is_integral_v<Integer>)
    std::string to_hex_string(const Integer& i, const bool uppercase = false)
    {
        return to_hex_string(&i, sizeof(Integer), uppercase);
    }

    inline std::string to_hex_string(const std::span<const std::byte> data, const bool uppercase = false)
    {
        return to_hex_string(data.data(), data.size(), uppercase);
    }

    inline std::byte parse_nibble(const char nibble)
    {
        const auto lower = char_to_lower(nibble);

        if (lower >= '0' && lower <= '9')
        {
            return static_cast<std::byte>(lower - '0');
        }

        if (lower >= 'a' && lower <= 'f')
        {
            return static_cast<std::byte>(10 + (lower - 'a'));
        }

        return static_cast<std::byte>(0);
    }

    inline std::vector<std::byte> from_hex_string(const std::string_view str)
    {
        const auto size = str.size() / 2;

        std::vector<std::byte> data{};
        data.reserve(size);

        for (size_t i = 0; i < size; ++i)
        {
            const auto high = parse_nibble(str[i * 2 + 0]);
            const auto low = parse_nibble(str[i * 2 + 1]);
            const auto value = (high << 4) | low;

            data.push_back(value);
        }

        return data;
    }

    template <class Elem, class Traits, class Alloc>
    bool equals_ignore_case(const std::basic_string<Elem, Traits, Alloc>& lhs, const std::basic_string<Elem, Traits, Alloc>& rhs)
    {
        return std::ranges::equal(lhs, rhs, [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }

    template <class Elem, class Traits>
    bool equals_ignore_case(const std::basic_string_view<Elem, Traits>& lhs, const std::basic_string_view<Elem, Traits>& rhs)
    {
        return std::ranges::equal(lhs, rhs, [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }

    template <class Elem, class Traits, class Alloc>
    bool starts_with_ignore_case(const std::basic_string<Elem, Traits, Alloc>& lhs, const std::basic_string<Elem, Traits, Alloc>& rhs)
    {
        if (lhs.length() < rhs.length())
        {
            return false;
        }

        return std::ranges::equal(lhs.substr(0, rhs.length()), rhs,
                                  [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }

    template <class Elem, class Traits>
    bool starts_with_ignore_case(const std::basic_string_view<Elem, Traits>& lhs, const std::basic_string_view<Elem, Traits>& rhs)
    {
        if (lhs.length() < rhs.length())
        {
            return false;
        }

        return std::ranges::equal(lhs.substr(0, rhs.length()), rhs,
                                  [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }

    template <class Elem, class Traits, class Alloc>
    bool ends_with_ignore_case(const std::basic_string<Elem, Traits, Alloc>& lhs, const std::basic_string<Elem, Traits, Alloc>& rhs)
    {
        if (lhs.length() < rhs.length())
        {
            return false;
        }

        auto start = lhs.length() - rhs.length();
        return std::ranges::equal(lhs.substr(start, rhs.length()), rhs,
                                  [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }

    template <class Elem, class Traits>
    bool ends_with_ignore_case(const std::basic_string_view<Elem, Traits>& lhs, const std::basic_string_view<Elem, Traits>& rhs)
    {
        if (lhs.length() < rhs.length())
        {
            return false;
        }

        auto start = lhs.length() - rhs.length();
        return std::ranges::equal(lhs.substr(start, rhs.length()), rhs,
                                  [](const auto c1, const auto c2) { return char_to_lower(c1) == char_to_lower(c2); });
    }
}
