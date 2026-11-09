#pragma once

#include "registry_manager.hpp"

#include <functional>
#include <string>

namespace sogen
{
    // Rewrites the string values a registry file sets before they are stored, such as to expand environment variables.
    using registry_string_transform = std::function<std::u16string(std::u16string_view)>;

    void import_registry_file(registry_manager& registry, const std::filesystem::path& file,
                              const registry_string_transform& transform = {});
    void import_registry_file_contents(registry_manager& registry, std::span<const std::byte> contents,
                                       std::string_view source_name = "<memory>", const registry_string_transform& transform = {});
}
