#pragma once

#include "registry_manager.hpp"

namespace sogen
{
    void import_registry_file(registry_manager& registry, const std::filesystem::path& file);
    void import_registry_file_contents(registry_manager& registry, std::span<const std::byte> contents,
                                       std::string_view source_name = "<memory>");
}
