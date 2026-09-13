#pragma once

#include "std_include.hpp"

namespace sogen
{
    struct macos_execution_context
    {
        uint64_t pc{};
        std::string module{"<N/A>"};

        // Which thread made the call. A multithreaded guest interleaves its syscalls, and a trace that
        // does not say who called what cannot be read at all once a workqueue pool is running.
        uint64_t thread_id{};
    };

    struct macos_run_started_event
    {
        std::string backend_name{};
        std::string application{};
    };

    struct macos_run_finished_event
    {
        bool success{};
        std::optional<int> exit_status{};
        uint64_t instructions{};
        std::string stop_detail{};
    };

    struct macos_run_failed_event
    {
        uint64_t pc{};
        std::string message{};
    };

    struct macos_syscall_event
    {
        uint64_t call_count{};
        uint64_t syscall_id{};
        std::string syscall_name{};
        bool is_mach_trap{};
        macos_execution_context execution{};
    };

    struct macos_syscall_error_event
    {
        std::string syscall_name{};
        int64_t error{};
        std::string error_name{};
    };

    struct macos_trace_detail_event
    {
        std::string label{};
        std::string value{};
    };

    struct macos_stdout_chunk_event
    {
        std::string data{};
        bool is_stderr{};
    };

    struct macos_generic_access_event
    {
        std::string type{};
        std::string name{};
    };

    struct macos_generic_activity_event
    {
        std::string details{};
    };

    struct macos_suspicious_activity_event
    {
        std::string details{};
        macos_execution_context execution{};
    };

    struct macos_memory_allocate_event
    {
        uint64_t address{};
        uint64_t length{};
        std::string permissions{};
        bool commit{};
    };

    struct macos_memory_protect_event
    {
        uint64_t address{};
        uint64_t length{};
        std::string permissions{};
    };

    struct macos_memory_release_event
    {
        uint64_t address{};
        uint64_t length{};
    };

    struct macos_module_load_event
    {
        std::string path{};
        uint64_t image_base{};
        uint64_t image_size{};
    };

    struct macos_dyld_image_event
    {
        std::string path{};
        uint64_t image_base{};
        uint64_t image_size{};
    };

    struct macos_thread_create_event
    {
        uint64_t thread_id{};
        uint64_t start_address{};
        uint64_t argument{};
    };

    struct macos_thread_terminated_event
    {
        uint64_t thread_id{};
    };

    struct macos_mach_port_event
    {
        uint32_t port_name{};
        std::string right{};
        std::string description{};
    };

    struct macos_mach_message_event
    {
        uint32_t remote_port{};
        uint32_t message_id{};
        std::string subsystem{};
    };

    struct macos_process_exit_event
    {
        int exit_status{};
    };

    struct macos_cpu_exception_event
    {
        uint64_t address{};
        int exception_index{};
        std::string description{};
    };

    using macos_analysis_event =
        std::variant<macos_run_started_event, macos_run_finished_event, macos_run_failed_event, macos_syscall_event,
                     macos_syscall_error_event, macos_trace_detail_event, macos_stdout_chunk_event, macos_generic_access_event,
                     macos_generic_activity_event, macos_suspicious_activity_event, macos_memory_allocate_event, macos_memory_protect_event,
                     macos_memory_release_event, macos_module_load_event, macos_dyld_image_event, macos_thread_create_event,
                     macos_thread_terminated_event, macos_mach_port_event, macos_mach_message_event, macos_process_exit_event,
                     macos_cpu_exception_event>;
}
