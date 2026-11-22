#pragma once

#include <filesystem>
#include <string_view>

namespace sogen::symbols
{
    void extract_cab_file(const std::filesystem::path& cabinet_path, const std::filesystem::path& output_path, std::string_view filename);
}
