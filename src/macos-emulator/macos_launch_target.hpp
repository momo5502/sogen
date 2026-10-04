#pragma once

#include "macos_input.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <guest/guest_file_system.hpp>

namespace sogen
{
    constexpr std::string_view MACOS_BUNDLE_GUEST_PREFIX = "/Applications";

    struct macos_bundle_mapping
    {
        std::string guest_root{};
        std::filesystem::path host_root{};
    };

    struct macos_launch_target
    {
        macos_input_kind kind{macos_input_kind::unknown};
        std::filesystem::path host_executable{};
        std::string guest_executable{};
        std::string working_directory{"/"};
        std::string bundle_identifier{};
        std::optional<macos_bundle_mapping> bundle{};
        std::string diagnostic{};

        bool runnable() const
        {
            return this->diagnostic.empty() && !this->guest_executable.empty();
        }
    };

    macos_launch_target resolve_macos_launch_target(const std::filesystem::path& input);

    void apply_macos_launch_target(const macos_launch_target& target, guest_file_system& fs);
}
