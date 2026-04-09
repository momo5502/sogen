#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class syscall_classification
{
    regular,
    inline_syscall,
    crafted_out_of_line,
};

struct event_header
{
    uint64_t sequence{};
    uint64_t instruction_count{};
};

struct execution_context
{
    uint32_t thread_id{};
    uint64_t rip{};
    std::string rip_module{"<N/A>"};
    std::optional<uint64_t> previous_ip{};
    std::optional<std::string> previous_ip_module{};
};

struct observation_event
{
    event_header header{};
    execution_context execution{};
};

struct summary_event
{
    event_header header{};
};

struct run_started_event : summary_event
{
    std::string backend_name{};
    std::string mode{"application"};
    std::string application{};
    std::vector<std::string> arguments{};
};

struct run_finished_event : summary_event
{
    bool success{};
    std::optional<uint32_t> exit_status{};
};

struct run_failed_event : summary_event
{
    uint64_t rip{};
    std::string message{};
};

struct instruction_summary_entry
{
    std::string mnemonic{};
    uint64_t count{};
};

struct instruction_summary_event : summary_event
{
    std::vector<instruction_summary_entry> entries{};
};

struct buffered_stdout_event : summary_event
{
    std::string data{};
};

struct stdout_chunk_event : observation_event
{
    std::string data{};
};

struct suspicious_activity_event : observation_event
{
    std::string details{};
    std::string decoded_instruction{};
};

struct debug_string_event : observation_event
{
    std::string details{};
};

struct generic_activity_event : observation_event
{
    std::string details{};
};

struct generic_access_event : observation_event
{
    std::string type{};
    std::string name{};
};

struct memory_allocate_event : observation_event
{
    uint64_t address{};
    uint64_t length{};
    std::string permissions{};
    bool commit{};
};

struct memory_protect_event : observation_event
{
    uint64_t address{};
    uint64_t length{};
    std::string permissions{};
};

struct memory_violation_event : observation_event
{
    uint64_t address{};
    uint64_t size{};
    std::string operation{};
    std::string violation_type{};
};

struct io_control_event : observation_event
{
    std::string device_name{};
    uint32_t code{};
};

struct thread_create_event : observation_event
{
    uint32_t created_thread_id{};
    uint64_t start_address{};
    uint64_t argument{};
    std::vector<std::string> flags{};
};

struct thread_terminated_event : observation_event
{
    uint32_t terminated_thread_id{};
};

struct thread_set_name_event : observation_event
{
    uint32_t renamed_thread_id{};
    std::string name{};
};

struct thread_switch_event : observation_event
{
    uint32_t previous_thread_id{};
    uint32_t next_thread_id{};
};

struct module_load_event : observation_event
{
    std::string path{};
    uint64_t image_base{};
};

struct module_unload_event : observation_event
{
    std::string path{};
    uint64_t image_base{};
};

struct import_read_event : observation_event
{
    uint64_t resolved_address{};
    std::string import_name{};
    std::string import_module{};
};

struct import_write_event : observation_event
{
    size_t size{};
    uint64_t value{};
    std::string import_name{};
    std::string import_module{};
};

struct object_access_event : observation_event
{
    bool main_access{};
    std::string type_name{};
    uint64_t offset{};
    uint64_t size{};
    std::optional<std::string> member_name{};
};

struct environment_access_event : observation_event
{
    bool main_access{};
    uint64_t offset{};
    uint64_t size{};
};

struct function_execution_detail
{
    std::string label{};
    std::string value{};
};

struct function_execution_event : observation_event
{
    std::string function_name{};
    bool interesting{};
    std::vector<function_execution_detail> details{};
    bool stubbed_win_verify_trust{};
};

struct entry_point_execution_event : observation_event
{
    bool interesting{};
};

struct foreign_code_transition_event : observation_event
{
    std::string function_name{};
    uint64_t function_offset{};
    bool interesting{};
};

struct section_first_execute_event : observation_event
{
    std::string module_name{};
    std::string section_name{};
    uint64_t file_address{};
};

struct rdtsc_event : observation_event
{
};

struct rdtscp_event : observation_event
{
};

struct cpuid_event : observation_event
{
    uint32_t leaf{};
};

struct syscall_event : observation_event
{
    syscall_classification classification{syscall_classification::regular};
    uint32_t syscall_id{};
    std::string syscall_name{};
    std::optional<uint64_t> caller_rip{};
    std::optional<std::string> caller_module{};
};

struct foreign_module_read_event : observation_event
{
    uint64_t address{};
    size_t size{};
    std::string module_name{};
    std::string region_name{};
};

struct executable_read_event : observation_event
{
    uint64_t address{};
    size_t size{};
    std::string section_name{};
};

struct executable_write_event : observation_event
{
    uint64_t address{};
    size_t size{};
    uint64_t value{};
    std::string section_name{};
};

using analysis_event =
    std::variant<run_started_event, run_finished_event, run_failed_event, instruction_summary_event, buffered_stdout_event,
                 stdout_chunk_event, suspicious_activity_event, debug_string_event, generic_activity_event, generic_access_event,
                 memory_allocate_event, memory_protect_event, memory_violation_event, io_control_event, thread_create_event,
                 thread_terminated_event, thread_set_name_event, thread_switch_event, module_load_event, module_unload_event,
                 import_read_event, import_write_event, object_access_event, environment_access_event, function_execution_event,
                 entry_point_execution_event, foreign_code_transition_event, section_first_execute_event, rdtsc_event, rdtscp_event,
                 cpuid_event, syscall_event, foreign_module_read_event, executable_read_event, executable_write_event>;
