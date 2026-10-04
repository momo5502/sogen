#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sogen
{
    constexpr size_t MAX_PLIST_SIZE = 4u * 1024u * 1024u;

    std::optional<std::string> plist_top_level_string(std::span<const std::byte> data, std::string_view key);
}
