#pragma once

#include <cstdint>
#include <filesystem>
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

template <typename Payload>
struct observation_event
{
    event_header header{};
    execution_context execution{};
    Payload payload{};
};

template <typename Payload>
struct summary_event
{
    event_header header{};
    Payload payload{};
};

struct run_started_payload
{
    std::string backend_name{};
    std::string mode{"application"};
    std::string application{};
    std::vector<std::string> arguments{};
};

struct run_finished_payload
{
    bool success{};
    std::optional<uint32_t> exit_status{};
};

struct run_failed_payload
{
    uint64_t rip{};
    std::string message{};
};

struct instruction_summary_entry
{
    std::string mnemonic{};
    uint64_t count{};
};

struct instruction_summary_payload
{
    std::vector<instruction_summary_entry> entries{};
};

struct buffered_stdout_payload
{
    std::string data{};
};

struct stdout_chunk_payload
{
    std::string data{};
};

struct suspicious_activity_payload
{
    std::string details{};
    std::string decoded_instruction{};
};

struct debug_string_payload
{
    std::string details{};
};

struct generic_activity_payload
{
    std::string details{};
};

struct generic_access_payload
{
    std::string type{};
    std::string name{};
};

struct memory_allocate_payload
{
    uint64_t address{};
    uint64_t length{};
    std::string permissions{};
    bool commit{};
};

struct memory_protect_payload
{
    uint64_t address{};
    uint64_t length{};
    std::string permissions{};
};

struct memory_violation_payload
{
    uint64_t address{};
    uint64_t size{};
    std::string operation{};
    std::string violation_type{};
};

struct io_control_payload
{
    std::string device_name{};
    uint32_t code{};
};

struct thread_create_payload
{
    uint32_t created_thread_id{};
    uint64_t start_address{};
    uint64_t argument{};
    std::vector<std::string> flags{};
};

struct thread_terminated_payload
{
    uint32_t terminated_thread_id{};
};

struct thread_set_name_payload
{
    uint32_t renamed_thread_id{};
    std::string name{};
};

struct thread_switch_payload
{
    uint32_t previous_thread_id{};
    uint32_t next_thread_id{};
};

struct module_load_payload
{
    std::string path{};
    uint64_t image_base{};
};

struct module_unload_payload
{
    std::string path{};
    uint64_t image_base{};
};

struct import_read_payload
{
    uint64_t resolved_address{};
    std::string import_name{};
    std::string import_module{};
};

struct import_write_payload
{
    size_t size{};
    uint64_t value{};
    std::string import_name{};
    std::string import_module{};
};

struct object_access_payload
{
    bool main_access{};
    std::string type_name{};
    uint64_t offset{};
    uint64_t size{};
    std::optional<std::string> member_name{};
};

struct environment_access_payload
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

struct function_execution_payload
{
    std::string function_name{};
    bool interesting{};
    std::vector<function_execution_detail> details{};
    bool stubbed_win_verify_trust{};
};

struct entry_point_execution_payload
{
    bool interesting{};
};

struct foreign_code_transition_payload
{
    std::string function_name{};
    uint64_t function_offset{};
    bool interesting{};
};

struct section_first_execute_payload
{
    std::string module_name{};
    std::string section_name{};
    uint64_t file_address{};
};

struct rdtsc_payload
{
};

struct rdtscp_payload
{
};

struct cpuid_payload
{
    uint32_t leaf{};
};

struct syscall_payload
{
    syscall_classification classification{syscall_classification::regular};
    uint32_t syscall_id{};
    std::string syscall_name{};
    std::optional<uint64_t> caller_rip{};
    std::optional<std::string> caller_module{};
};

struct foreign_module_read_payload
{
    uint64_t address{};
    size_t size{};
    std::string module_name{};
    std::string region_name{};
};

struct executable_read_payload
{
    uint64_t address{};
    size_t size{};
    std::string section_name{};
};

struct executable_write_payload
{
    uint64_t address{};
    size_t size{};
    uint64_t value{};
    std::string section_name{};
};

using run_started_event = summary_event<run_started_payload>;
using run_finished_event = summary_event<run_finished_payload>;
using run_failed_event = summary_event<run_failed_payload>;
using instruction_summary_event = summary_event<instruction_summary_payload>;
using buffered_stdout_event = summary_event<buffered_stdout_payload>;

using stdout_chunk_event = observation_event<stdout_chunk_payload>;
using suspicious_activity_event = observation_event<suspicious_activity_payload>;
using debug_string_event = observation_event<debug_string_payload>;
using generic_activity_event = observation_event<generic_activity_payload>;
using generic_access_event = observation_event<generic_access_payload>;
using memory_allocate_event = observation_event<memory_allocate_payload>;
using memory_protect_event = observation_event<memory_protect_payload>;
using memory_violation_event = observation_event<memory_violation_payload>;
using io_control_event = observation_event<io_control_payload>;
using thread_create_event = observation_event<thread_create_payload>;
using thread_terminated_event = observation_event<thread_terminated_payload>;
using thread_set_name_event = observation_event<thread_set_name_payload>;
using thread_switch_event = observation_event<thread_switch_payload>;
using module_load_event = observation_event<module_load_payload>;
using module_unload_event = observation_event<module_unload_payload>;
using import_read_event = observation_event<import_read_payload>;
using import_write_event = observation_event<import_write_payload>;
using object_access_event = observation_event<object_access_payload>;
using environment_access_event = observation_event<environment_access_payload>;
using function_execution_event = observation_event<function_execution_payload>;
using entry_point_execution_event = observation_event<entry_point_execution_payload>;
using foreign_code_transition_event = observation_event<foreign_code_transition_payload>;
using section_first_execute_event = observation_event<section_first_execute_payload>;
using rdtsc_event = observation_event<rdtsc_payload>;
using rdtscp_event = observation_event<rdtscp_payload>;
using cpuid_event = observation_event<cpuid_payload>;
using syscall_event = observation_event<syscall_payload>;
using foreign_module_read_event = observation_event<foreign_module_read_payload>;
using executable_read_event = observation_event<executable_read_payload>;
using executable_write_event = observation_event<executable_write_payload>;

using analysis_event =
    std::variant<run_started_event, run_finished_event, run_failed_event, instruction_summary_event, buffered_stdout_event,
                 stdout_chunk_event, suspicious_activity_event, debug_string_event, generic_activity_event, generic_access_event,
                 memory_allocate_event, memory_protect_event, memory_violation_event, io_control_event, thread_create_event,
                 thread_terminated_event, thread_set_name_event, thread_switch_event, module_load_event, module_unload_event,
                 import_read_event, import_write_event, object_access_event, environment_access_event, function_execution_event,
                 entry_point_execution_event, foreign_code_transition_event, section_first_execute_event, rdtsc_event, rdtscp_event,
                 cpuid_event, syscall_event, foreign_module_read_event, executable_read_event, executable_write_event>;
