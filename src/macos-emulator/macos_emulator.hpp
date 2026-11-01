#pragma once

#include <arch_emulator.hpp>
#include <hook_interface.hpp>
#include <logger.hpp>
#include <posix_child_processes.hpp>
#include <posix_fd_table.hpp>
#include <posix_file_system.hpp>
#include <posix_memory_manager.hpp>

#include "macos_file_ids.hpp"
#include "macos_mach_ports.hpp"
#include "macos_shared_region.hpp"
#include "macos_signals.hpp"
#include "macos_syscall_context.hpp"
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
    using x86_64_emulator_factory = std::function<std::unique_ptr<x86_64_emulator>()>;

    bool is_macos_executable(std::span<const std::byte> image);

    struct macos_process_identity
    {
        uint32_t process_id{MACOS_PROCESS_ID};
        uint32_t parent_process_id{1};
        uint32_t process_group_id{MACOS_PROCESS_ID};
        uint64_t unique_id{MACOS_PROCESS_UNIQUE_ID};
        uint64_t parent_unique_id{1};
    };

    // What the processes of one emulation share, as they would share a kernel.
    struct macos_system
    {
        x86_64_emulator_factory create_backend{};
        uint32_t last_process_id{MACOS_PROCESS_ID};
        uint64_t last_unique_id{MACOS_PROCESS_UNIQUE_ID};
    };

    class macos_emulator
    {
        struct forked_tag
        {
        };

        std::shared_ptr<macos_system> system_{};
        std::unique_ptr<x86_64_emulator> emu_{};

      public:
        logger log{};
        posix_memory_manager memory;
        posix_file_system file_system;
        posix_fd_table fds{};
        macos_file_ids file_ids{};
        macos_mach_ports mach_ports{};
        macos_syscall_dispatcher dispatcher{};
        std::shared_ptr<macos_shared_region> shared_region{};
        std::optional<int> exit_status{};
        macos_signal_state signals{};
        macos_process_identity identity{};
        posix_child_processes children{};
        std::array<uint8_t, 16> executable_uuid{};
        std::string process_name{};
        std::string working_directory{"/"};

        uint64_t next_entropy();

        std::string resolve_guest_path(const std::string_view guest_path) const
        {
            return posix_file_system::resolve_guest_path_string(this->working_directory, guest_path);
        }

        std::function<void(std::string_view data)> on_stdout{};
        std::function<void(std::string_view data)> on_stderr{};

        macos_emulator(x86_64_emulator_factory create_backend, std::filesystem::path emulation_root,
                       const std::filesystem::path& executable, const std::vector<std::string>& argv, const std::vector<std::string>& envp);
        macos_emulator(x86_64_emulator_factory create_backend, std::filesystem::path emulation_root, std::span<const std::byte> image,
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

        // Runs once the CPU has left the current syscall, for work that replaces or copies the running image.
        void after_syscall(std::function<void()> operation);

        void exec_image(std::span<const std::byte> image, const std::string& executable_path, const std::vector<std::string>& argv,
                        const std::vector<std::string>& envp);
        uint32_t fork_process(const std::function<void(macos_emulator& child)>& prepare_child);

        int wait_status() const;

      private:
        bool should_stop_{false};
        uint64_t entropy_state_{0x5EED5EED5EED5EED};
        std::function<void()> after_syscall_{};

        macos_emulator(forked_tag, macos_emulator& parent);

        void load_image(std::span<const std::byte> image, const std::string& executable_path, const std::vector<std::string>& argv,
                        const std::vector<std::string>& envp);
        void setup_hooks();
    };
}
