#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sogen
{
    struct macos_bundle
    {
        std::filesystem::path bundle_root{};
        std::filesystem::path executable{};
        std::string bundle_name{};
        std::string executable_name{};
        std::string identifier{};
    };

    bool is_app_bundle_path(const std::filesystem::path& path);

    std::optional<std::filesystem::path> enclosing_app_bundle(const std::filesystem::path& executable);

    std::optional<macos_bundle> resolve_app_bundle(const std::filesystem::path& path, std::string& error);
}
