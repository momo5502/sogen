#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sogen
{
    struct managed_process
    {
        uint64_t value{};

        explicit operator bool() const
        {
            return this->value != 0;
        }

        auto operator<=>(const managed_process&) const = default;
    };

    enum class process_error
    {
        none,
        invalid_process,
        unavailable,
        permission_denied,
        resource_limit,
        communication_failure,
        internal_failure,
    };

    enum class process_exit_kind
    {
        exited,
        terminated,
        runtime_failure,
    };

    struct process_exit
    {
        process_exit_kind kind{process_exit_kind::exited};
        uint64_t code{};
    };

    struct process_create_request
    {
        std::string application{};
        std::string working_directory{};
        std::vector<std::string> arguments{};
        std::unordered_map<std::string, std::string> environment{};
    };

    struct process_create_result
    {
        managed_process process{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_exit_status_result
    {
        std::optional<process_exit> status{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_manager
    {
        virtual ~process_manager() = default;

        virtual process_create_result create_process(process_create_request request) = 0;
        virtual process_error terminate_process(managed_process process, uint64_t exit_code) = 0;
        virtual process_exit_status_result exit_status(managed_process process) const = 0;
    };
}
