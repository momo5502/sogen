#pragma once

#include "std_include.hpp"

#include <hook_interface.hpp>
#include <utils/function.hpp>

#include "module/elf_mapping.hpp"
#include "linux_syscall_info.hpp"

namespace sogen
{

    enum class linux_syscall_hook_continuation : uint8_t
    {
        run_handler,
        skip_handler,
        stop,
    };

    struct linux_emulator_callbacks
    {
        utils::optional_function<void(std::string_view data)> on_stdout{};
        utils::optional_function<void(std::string_view data)> on_stderr{};
        utils::optional_function<linux_syscall_hook_continuation(const linux_syscall_info& info)> on_syscall{};
        utils::callback_list<void(const linux_mapped_module& module)> on_module_load{};
        utils::optional_function<void(uint64_t address)> on_instruction{};
        utils::optional_function<memory_violation_continuation(uint64_t address, size_t size, memory_operation operation,
                                                               memory_violation_type type)>
            on_memory_violate{};
    };

} // namespace sogen
