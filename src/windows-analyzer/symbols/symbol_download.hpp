#pragma once

#include <filesystem>
#include <string>

namespace sogen
{
    struct symbol_download_result
    {
        bool success{};
        std::string reason{};
        bool not_found{};
    };

    symbol_download_result download_symbol(const std::string& url, const std::filesystem::path& output_path);
}
