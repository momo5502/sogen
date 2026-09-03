#pragma once

#include <filesystem>
#include <string>

namespace sogen
{
    enum class macos_input_kind
    {
        missing,
        special_file,
        directory,
        app_bundle,
        mach_o,
        fat_mach_o,
        disk_image,
        encrypted_disk_image,
        unknown,
    };

    macos_input_kind classify_macos_input(const std::filesystem::path& path);

    std::string describe_unsupported_input(const std::filesystem::path& path, macos_input_kind kind);
}
