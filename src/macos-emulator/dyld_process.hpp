#pragma once

#include "std_include.hpp"

#include "macos_file_identity.hpp"
#include "macos_memory_manager.hpp"
#include "macos_platform.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    struct macos_apple_strings
    {
        std::string executable_path{};
        uint64_t pfz{MACOS_COMMPAGE_BASE};
        uint64_t stack_guard{};
        std::array<uint64_t, 2> malloc_entropy{};
        uint64_t ptr_munge{};
        uint64_t main_stack_top{MACOS_MAIN_STACK_TOP};
        uint64_t main_stack_size{MACOS_MAIN_STACK_SIZE};
        uint64_t main_stack_alloc_base{MACOS_MAIN_STACK_TOP - MACOS_MAIN_STACK_SIZE};
        uint64_t main_stack_alloc_size{MACOS_MAIN_STACK_SIZE};
        macos_file_identity executable_file{};
        macos_file_identity dyld_file{};
        std::string executable_cdhash{};
        std::string executable_boothash{};
        uint32_t th_port{};
        uint64_t security_config{};

        std::vector<std::string> render() const;
    };

    struct macos_kernel_args_layout
    {
        uint64_t stack_pointer{};
        uint64_t mach_header{};
        uint64_t argc{};
        uint64_t argv{};
        uint64_t envp{};
        uint64_t apple{};
        bool valid{};
    };

    macos_kernel_args_layout build_dyld_kernel_args(macos_memory_manager& memory, uint64_t stack_top, uint64_t stack_base,
                                                    uint64_t mach_header, const std::vector<std::string>& argv,
                                                    const std::vector<std::string>& envp, const std::vector<std::string>& apple);

    std::string format_hex_key(std::string_view key, uint64_t value);
    std::string format_padded_hex_key(std::string_view key, uint64_t value);
}
