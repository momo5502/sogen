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

    template <typename T>
    using unordered_string_map = std::unordered_map<std::string, T, string_hash, std::equal_to<>>;

    using unordered_string_set = std::unordered_set<std::string, string_hash, std::equal_to<>>;
}
