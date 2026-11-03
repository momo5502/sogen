#pragma once

#include "mount_provider.hpp"

namespace sogen
{

    // A host directory mounted copy-on-write: the guest's changes land in the upper and state directories, and the
    // directory itself is only read. Symlinks inside it are ignored, so that the guest cannot reach outside it.
    struct directory_mount
    {
        windows_path guest_path{};
        std::filesystem::path host_directory{};
        std::filesystem::path upper_directory{};
        std::filesystem::path state_directory{};

        static constexpr size_t argument_count = 4;

        std::vector<std::string> to_arguments() const
        {
            return {this->guest_path.string(), this->host_directory.string(), this->upper_directory.string(),
                    this->state_directory.string()};
        }

        static directory_mount from_arguments(const std::span<const std::string> arguments)
        {
            if (arguments.size() != argument_count)
            {
                throw std::runtime_error("A directory mount takes " + std::to_string(argument_count) + " values");
            }

            return {
                .guest_path = windows_path{arguments[0]},
                .host_directory = arguments[1],
                .upper_directory = arguments[2],
                .state_directory = arguments[3],
            };
        }
    };

    std::unique_ptr<mount_provider> create_directory_mount(const directory_mount& mount);

} // namespace sogen
