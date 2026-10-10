#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <logger.hpp>
#include <posix_file_system.hpp>
#include <posix_memory_manager.hpp>

#include "macos_syscall_dispatcher.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    class macos_emulator
    {
        std::unique_ptr<x86_64_emulator> emu_{};

      public:
        logger log{};
        posix_memory_manager memory;
        posix_file_system file_system;
        macos_syscall_dispatcher dispatcher{};
        std::optional<int> exit_status{};

        std::function<void(std::string_view data)> on_stdout{};
        std::function<void(std::string_view data)> on_stderr{};

        macos_emulator(std::unique_ptr<x86_64_emulator> emu, std::filesystem::path emulation_root, const std::filesystem::path& executable,
                       const std::vector<std::string>& argv, const std::vector<std::string>& envp);
        macos_emulator(std::unique_ptr<x86_64_emulator> emu, std::filesystem::path emulation_root, std::span<const std::byte> image,
                       const std::string& executable_path, const std::vector<std::string>& argv, const std::vector<std::string>& envp);

        macos_emulator(const macos_emulator&) = delete;
        macos_emulator& operator=(const macos_emulator&) = delete;
        macos_emulator(macos_emulator&&) = delete;
        macos_emulator& operator=(macos_emulator&&) = delete;

        ~macos_emulator() = default;

        x86_64_emulator& emu()
        {
            return *this->emu_;
        }

        void start();
        void stop();

        uint32_t allocate_mach_port_name();

      private:
        uint32_t next_mach_port_name_{0x303};

        void setup_hooks();
    };
}
