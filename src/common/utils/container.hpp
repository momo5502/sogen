#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include "string.hpp"

namespace utils
{
    template <typename Elem, typename Traits>
    struct basic_string_hash
    {
        using is_transparent = void;

        size_t operator()(const std::basic_string_view<Elem, Traits> str) const
        {
            constexpr std::hash<std::basic_string_view<Elem, Traits>> hasher{};
            return hasher(str);
        }
    };

    template <typename Elem, typename Traits>
    struct basic_insensitive_string_hash
    {
        using is_transparent = void;

        size_t operator()(const std::basic_string_view<Elem, Traits> str) const
        {
            size_t hash = 0;
            constexpr std::hash<int> hasher{};
            for (const auto c : str)
            {
                hash ^= hasher(string::char_to_lower(c)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    template <typename Elem, typename Traits>
    struct basic_insensitive_string_equal
    {
        using is_transparent = void;

        bool operator()(const std::basic_string_view<Elem, Traits> lhs, const std::basic_string_view<Elem, Traits> rhs) const
        {
            return string::equals_ignore_case(lhs, rhs);
        }
    };

    using string_hash = basic_string_hash<char, std::char_traits<char>>;
    using u16string_hash = basic_string_hash<char16_t, std::char_traits<char16_t>>;

    using insensitive_string_hash = basic_insensitive_string_hash<char, std::char_traits<char>>;
    using insensitive_u16string_hash = basic_insensitive_string_hash<char16_t, std::char_traits<char16_t>>;

    using insensitive_string_equal = basic_insensitive_string_equal<char, std::char_traits<char>>;
    using insensitive_u16string_equal = basic_insensitive_string_equal<char16_t, std::char_traits<char16_t>>;

    template <typename T>
    using unordered_string_map = std::unordered_map<std::string, T, string_hash, std::equal_to<>>;
    template <typename T>
    using unordered_u16string_map = std::unordered_map<std::u16string, T, u16string_hash, std::equal_to<>>;

    template <typename T>
    using unordered_insensitive_string_map = std::unordered_map<std::string, T, insensitive_string_hash, insensitive_string_equal>;
    template <typename T>
    using unordered_insensitive_u16string_map =
        std::unordered_map<std::u16string, T, insensitive_u16string_hash, insensitive_u16string_equal>;

    using unordered_string_set = std::unordered_set<std::string, string_hash, std::equal_to<>>;
    using unordered_u16string_set = std::unordered_set<std::u16string, u16string_hash, std::equal_to<>>;
}
