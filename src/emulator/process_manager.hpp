#pragma once

#include "memory_permission.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
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

    struct process_memory_result
    {
        uint64_t address{};
        uint64_t size{};
        memory_permission permission{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_memory_read_result
    {
        std::vector<uint8_t> data{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
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
        uint64_t native_environment{};
        uint64_t compatibility_environment{};
        uint64_t native_parameters{};
        uint64_t compatibility_parameters{};
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

    struct managed_process_target
    {
        virtual ~managed_process_target() = default;

        virtual uint64_t native_environment() const = 0;
        virtual uint64_t compatibility_environment() const = 0;
        virtual uint64_t native_parameters() const = 0;
        virtual uint64_t compatibility_parameters() const = 0;
        virtual process_memory_result allocate_memory(uint64_t address, uint64_t size, memory_permission permission, bool reserve,
                                                      bool commit) = 0;
        virtual process_error free_memory(uint64_t address, uint64_t size, bool release) = 0;
        virtual process_memory_result protect_memory(uint64_t address, uint64_t size, memory_permission permission) = 0;
        virtual process_memory_read_result read_memory(uint64_t address, uint64_t size) const = 0;
        virtual process_memory_result write_memory(uint64_t address, std::span<const uint8_t> data) = 0;
    };

    struct process_manager
    {
        virtual ~process_manager() = default;

        virtual process_create_result create_process(process_create_request request) = 0;
        virtual process_error resume_process(managed_process process) = 0;
        virtual process_error terminate_process(managed_process process, uint64_t exit_code) = 0;
        virtual process_exit_status_result exit_status(managed_process process) const = 0;
        virtual process_memory_result allocate_memory(managed_process process, uint64_t address, uint64_t size,
                                                      memory_permission permission, bool reserve, bool commit) = 0;
        virtual process_error free_memory(managed_process process, uint64_t address, uint64_t size, bool release) = 0;
        virtual process_memory_result protect_memory(managed_process process, uint64_t address, uint64_t size,
                                                     memory_permission permission) = 0;
        virtual process_memory_read_result read_memory(managed_process process, uint64_t address, uint64_t size) const = 0;
        virtual process_memory_result write_memory(managed_process process, uint64_t address, std::span<const uint8_t> data) = 0;
    };
}
