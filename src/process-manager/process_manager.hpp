#pragma once

#include <chrono>

#include "memory_permission.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sogen
{
    inline constexpr uint32_t default_guest_process_id = 0x1000;

    namespace utils
    {
        class host_pipe;
    }

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
        uint64_t cycles{};
    };

    struct process_inherited_pipe
    {
        uint64_t guest_handle{};
        uint64_t file_id{};
        std::shared_ptr<utils::host_pipe> endpoint;
        std::string name;
        uint32_t access{};
        uint32_t pipe_type{};
        uint32_t read_mode{};
        uint32_t completion_mode{};
        uint32_t create_options{};
        uint32_t completion_notification_flags{};
        bool inherit{};
    };

    enum class process_shared_object_kind : uint8_t
    {
        section,
        event,
        mutant,
    };

    struct process_shared_object
    {
        process_shared_object_kind kind{};
        uint64_t guest_handle{};
        std::string memory{};
        uint64_t size{};
        uint32_t protection{};
        uint32_t attributes{};
        uint32_t event_type{};
        uint32_t access{};
        bool inherit{};
    };

    struct process_cycle_time_result
    {
        uint64_t cycles{};
        process_error error{process_error::none};
    };

    struct process_thread_token_result
    {
        uint64_t token{};
        process_error error{process_error::none};
    };

    struct process_object_install_result
    {
        uint64_t handle{};
        process_error error{process_error::none};
    };

    struct process_object_export_result
    {
        process_shared_object object{};
        process_error error{process_error::none};
    };

    struct process_create_request
    {
        std::string application{};
        std::string argument0{};
        std::string command_line{};
        std::string working_directory{};
        std::vector<std::string> arguments{};
        std::unordered_map<std::string, std::string> environment{};
        uint32_t process_id{default_guest_process_id};
        uint32_t thread_id{8};
        uint64_t standard_input{};
        uint64_t standard_output{};
        uint64_t standard_error{};
        std::vector<process_inherited_pipe> inherited_pipes{};
        std::vector<process_shared_object> inherited_objects{};
        bool debugged{};
    };

    struct process_create_result
    {
        managed_process process{};
        uint32_t process_id{};
        uint32_t thread_id{};
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

    enum class process_debug_event_kind
    {
        create_process,
        create_thread,
        exit_thread,
        exit_process,
        exception,
        load_dll,
        unload_dll,
    };

    // What a debugged child reports to its debugger; the child stops until the debugger continues the event.
    struct process_debug_event
    {
        process_debug_event_kind kind{process_debug_event_kind::create_process};
        uint32_t thread_id{};
        uint64_t base_address{};
        uint64_t start_address{};
        std::string path{};
        uint32_t exit_status{};
        uint32_t exception_code{};
        uint32_t exception_flags{};
        uint64_t exception_address{};
        std::vector<uint64_t> exception_parameters{};
        bool first_chance{};
    };

    struct process_debug_event_result
    {
        std::optional<process_debug_event> event{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_thread_context_result
    {
        std::vector<uint8_t> context{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_thread_information
    {
        uint64_t teb{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_suspend_result
    {
        uint32_t previous_count{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_memory_region
    {
        uint64_t base_address{};
        uint64_t allocation_base{};
        uint32_t allocation_protect{};
        uint64_t region_size{};
        uint32_t state{};
        uint32_t protect{};
        uint32_t type{};
        std::string mapped_file{};
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
        virtual process_debug_event_result next_debug_event() = 0;
        virtual process_error continue_debug_event(uint32_t thread_id, uint32_t status) = 0;
        virtual process_error detach_debugger() = 0;
        // wow64 selects the 32-bit WOW64_CONTEXT of a WOW64 thread instead of its native context.
        virtual process_thread_context_result get_thread_context(uint32_t thread_id, bool wow64) = 0;
        virtual process_error set_thread_context(uint32_t thread_id, std::span<const uint8_t> context, bool wow64) = 0;
        virtual process_thread_information thread_information(uint32_t thread_id) = 0;
        virtual process_suspend_result suspend_thread(uint32_t thread_id, bool resume) = 0;
        virtual process_memory_region query_memory(uint64_t address) const = 0;
        virtual process_object_install_result install_object(const process_shared_object& object) = 0;
        virtual process_object_export_result export_object(uint64_t handle) = 0;
        virtual process_error close_handle(uint64_t handle) = 0;
        virtual process_error set_thread_token(uint32_t thread_id, uint64_t token) = 0;
        virtual process_thread_token_result get_thread_token(uint32_t thread_id) = 0;
        virtual process_cycle_time_result cycle_time() const = 0;
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

        virtual process_debug_event_result next_debug_event(managed_process /*process*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_error continue_debug_event(managed_process /*process*/, uint32_t /*thread_id*/, uint32_t /*status*/)
        {
            return process_error::unavailable;
        }

        virtual process_error detach_debugger(managed_process /*process*/)
        {
            return process_error::unavailable;
        }

        virtual process_object_install_result install_object(managed_process /*process*/, const process_shared_object& /*object*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_object_export_result export_object(managed_process /*process*/, uint64_t /*handle*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_error close_handle(managed_process /*process*/, uint64_t /*handle*/)
        {
            return process_error::unavailable;
        }

        virtual process_error set_thread_token(managed_process /*process*/, uint32_t /*thread_id*/, uint64_t /*token*/)
        {
            return process_error::unavailable;
        }

        virtual process_thread_token_result get_thread_token(managed_process /*process*/, uint32_t /*thread_id*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_cycle_time_result cycle_time(managed_process /*process*/) const
        {
            return {.error = process_error::unavailable};
        }

        // Child processes outlive their parent on Windows, so the host keeps them running until they exit on their own.
        virtual size_t running_process_count() const
        {
            return 0;
        }

        // Waits at most timeout for the children to exit on their own. False when some are still running, which the
        // caller answers by terminating them: an analysis run must not hang on a watchdog or a service a sample left
        // behind.
        virtual bool wait_for_processes(std::chrono::milliseconds /*timeout*/)
        {
            return true;
        }

        virtual void terminate_running_processes(uint64_t /*exit_code*/)
        {
        }

        virtual process_thread_context_result get_thread_context(managed_process /*process*/, uint32_t /*thread_id*/, bool /*wow64*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_error set_thread_context(managed_process /*process*/, uint32_t /*thread_id*/, std::span<const uint8_t> /*context*/,
                                                 bool /*wow64*/)
        {
            return process_error::unavailable;
        }

        virtual process_thread_information thread_information(managed_process /*process*/, uint32_t /*thread_id*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_suspend_result suspend_thread(managed_process /*process*/, uint32_t /*thread_id*/, bool /*resume*/)
        {
            return {.error = process_error::unavailable};
        }

        virtual process_memory_region query_memory(managed_process /*process*/, uint64_t /*address*/) const
        {
            return {.error = process_error::unavailable};
        }
    };
}
