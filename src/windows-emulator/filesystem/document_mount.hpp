#pragma once

#include "mount_provider.hpp"

namespace sogen
{

    // A document tree served by a document_broker of the parent process, mounted copy-on-write with the upper and state
    // directories the parent uses for it.
    struct document_mount
    {
        windows_path guest_path{};
        std::string broker{};
        std::string token{};
        std::filesystem::path upper_directory{};
        std::filesystem::path state_directory{};

        static constexpr size_t argument_count = 5;

        std::vector<std::string> to_arguments() const
        {
            return {this->guest_path.string(), this->broker, this->token, this->upper_directory.string(), this->state_directory.string()};
        }

        static document_mount from_arguments(const std::span<const std::string> arguments)
        {
            if (arguments.size() != argument_count)
            {
                throw std::runtime_error("A document mount takes " + std::to_string(argument_count) + " values");
            }

            return {
                .guest_path = windows_path{arguments[0]},
                .broker = arguments[1],
                .token = arguments[2],
                .upper_directory = arguments[3],
                .state_directory = arguments[4],
            };
        }
    };

    std::unique_ptr<mount_provider> connect_document_mount(const document_mount& mount);

} // namespace sogen
