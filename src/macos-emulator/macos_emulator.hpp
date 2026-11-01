#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <logger.hpp>
#include <posix_file_system.hpp>
#include <posix_memory_manager.hpp>

#include "macos_fd_table.hpp"
#include "macos_file_ids.hpp"
#include "macos_mach_ports.hpp"
#include "macos_shared_region.hpp"
#include "macos_syscall_dispatcher.hpp"

#include <array>
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
        macos_fd_table fds{};
        macos_file_ids file_ids{};
        macos_mach_ports mach_ports{};
        macos_syscall_dispatcher dispatcher{};
        std::optional<macos_shared_region> shared_region{};
        std::optional<int> exit_status{};
        uint32_t signal_mask{};
        std::array<uint8_t, 16> executable_uuid{};
        std::string process_name{};

        uint64_t next_entropy();

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

      private:
        bool should_stop_{false};
        uint64_t entropy_state_{0x5EED5EED5EED5EED};

        void setup_hooks();
    };
}
