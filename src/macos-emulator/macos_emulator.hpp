#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <logger.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sogen
{
    class macos_emulator
    {
      public:
        logger log{};
        std::optional<int> exit_status{};

        macos_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& executable, const std::vector<std::string>& argv,
                       const std::vector<std::string>& envp);

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
        std::unique_ptr<x86_64_emulator> emu_{};

        void setup_hooks();
        instruction_hook_continuation handle_syscall();
        void handle_write();
    };
}
