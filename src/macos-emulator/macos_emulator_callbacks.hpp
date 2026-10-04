#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <hook_interface.hpp>
#include <memory_permission.hpp>
#include <utils/function.hpp>

namespace sogen
{
    struct macos_mapped_module;

    struct macos_trace_detail
    {
        std::string label{};
        std::string value{};
    };

    struct macos_emulator_callbacks
    {
        template <typename T>
        using opt_func = utils::optional_function<T>;

        using continuation = instruction_hook_continuation;

        opt_func<continuation(uint64_t syscall_id, std::string_view syscall_name)> on_syscall{};
        opt_func<continuation(uint32_t trap_index, std::string_view trap_name)> on_mach_trap{};
        opt_func<void(std::string_view syscall_name, int64_t error, std::string_view error_name)> on_syscall_error{};
        opt_func<void(const macos_trace_detail& detail)> on_trace_detail{};

        // Whether the guest got a shared cache, and how much of one. Without this a front-end that shows
        // no console cannot tell "dyld mapped the cache" from "dyld gave up and fell back to loading
        // dylibs off disk", and the two look identical until a library that only exists inside the cache
        // fails to load.
        opt_func<void(uint32_t mappings, uint64_t rebased)> on_shared_cache_mapped{};

        // How much of the window path was actually intercepted. A front-end cannot tell a bound GUI from
        // an unbound one by watching the guest: with none bound the guest simply calls the real SkyLight,
        // which talks to a window server that is not there, and the failure surfaces somewhere else
        // entirely.
        opt_func<void(size_t bound, size_t registered, size_t unbound)> on_gui_routines_bound{};

        opt_func<void(std::string_view data)> on_stdout{};
        opt_func<void(std::string_view data)> on_stderr{};

        opt_func<void(std::string_view type, std::string_view name)> on_generic_access{};
        opt_func<void(std::string_view description)> on_generic_activity{};
        opt_func<void(std::string_view description)> on_suspicious_activity{};

        opt_func<void(uint64_t address, uint64_t length, memory_permission permissions, bool commit)> on_memory_allocate{};
        opt_func<void(uint64_t address, uint64_t length, memory_permission permissions)> on_memory_protect{};
        opt_func<void(uint64_t address, uint64_t length)> on_memory_release{};
        opt_func<void(uint64_t address, uint64_t size, memory_operation operation, memory_violation_type type)> on_memory_violate{};

        utils::callback_list<void(const macos_mapped_module& mod)> on_module_load{};
        utils::callback_list<void(const macos_mapped_module& mod)> on_module_unload{};
        opt_func<void(std::string_view path, uint64_t image_base, uint64_t image_size)> on_dyld_image{};

        opt_func<void(uint64_t thread_id, uint64_t start_address, uint64_t argument)> on_thread_create{};
        opt_func<void(uint64_t thread_id)> on_thread_terminated{};
        opt_func<void(uint64_t thread_id, std::string_view name)> on_thread_set_name{};

        opt_func<void(uint32_t port_name, std::string_view right, std::string_view description)> on_mach_port{};
        opt_func<void(uint32_t remote_port, uint32_t message_id, std::string_view subsystem)> on_mach_message{};

        opt_func<void(int exit_status)> on_process_exit{};
        opt_func<void(uint64_t address, int exception_index, std::string_view description)> on_cpu_exception{};
    };

    struct macos_trace_settings
    {
        bool decode_arguments{true};
        size_t string_limit{256};
        size_t buffer_preview_limit{32};
    };
}
