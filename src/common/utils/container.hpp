#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

namespace utils
{
    struct string_hash
    {
        using is_transparent = void;

        size_t operator()(const std::string_view str) const
        {
            constexpr std::hash<std::string_view> hasher{};
            return hasher(str);
        }
    };

    struct insensitive_string_hash
    {
        using is_transparent = void;

        size_t operator()(const std::string_view str) const
        {
            size_t hash = 0;
            constexpr std::hash<int> hasher{};
            for (unsigned char c : str)
            {
                hash ^= hasher(std::tolower(c)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    struct insensitive_string_equal
    {
        using is_transparent = void;

        bool operator()(const std::string_view lhs, const std::string_view rhs) const
        {
            return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                              [](unsigned char c1, unsigned char c2) { return std::tolower(c1) == std::tolower(c2); });
        }
    };

    template <typename T>
    using unordered_string_map = std::unordered_map<std::string, T, string_hash, std::equal_to<>>;

    template <typename T>
    using unordered_insensitive_string_map =
        std::unordered_map<std::string, T, insensitive_string_hash, insensitive_string_equal>;

    using unordered_string_set = std::unordered_set<std::string, string_hash, std::equal_to<>>;
}
