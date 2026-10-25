#pragma once

#include "windows_emulator.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace sogen
{

    namespace snapshot
    {
        // The configuration is the launcher's description of the host setup the process depends on (emulation root, mounts,
        // registry, network). Loading a snapshot under a different one fails instead of reopening files elsewhere.
        std::vector<std::byte> create_emulator_snapshot(windows_emulator& win_emu, std::string_view configuration = {});
        std::filesystem::path write_emulator_snapshot(windows_emulator& win_emu, bool log = true);
        void write_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                     std::string_view configuration = {}, const std::function<bool()>& should_commit = {});

        PEMachineType get_snapshot_machine(const std::filesystem::path& snapshot_file);

        void load_emulator_snapshot(windows_emulator& win_emu, std::span<const std::byte> snapshot, std::string_view configuration = {});
        void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                    std::string_view configuration = {});

        struct snapshot_outcome
        {
            std::filesystem::path path{};
            // Empty once the snapshot is written.
            std::string error{};
        };

        // Saves the running emulator when asked to from any thread. A process that presents frames is stopped after its
        // next present, or where it is if none comes within two seconds. A failed save keeps the process running.
        class controller
        {
          public:
            using outcome_callback = std::function<void(const snapshot_outcome& outcome)>;

            controller(windows_emulator& win_emu, std::string configuration, std::filesystem::path directory, outcome_callback on_outcome);
            ~controller();

            controller(const controller&) = delete;
            controller& operator=(const controller&) = delete;
            controller(controller&&) = delete;
            controller& operator=(controller&&) = delete;

            // An empty destination picks a new file in the directory. Returns false while another request is pending.
            bool request(std::filesystem::path destination = {});
            void stop();

            // Runs the emulator until the process exits, is stopped, or was saved, which returns true.
            bool run();

          private:
            void watch();
            std::filesystem::path next_snapshot_path() const;

            windows_emulator& win_emu_;
            std::string configuration_{};
            std::filesystem::path directory_{};
            outcome_callback on_outcome_{};

            std::mutex mutex_{};
            std::condition_variable changed_{};
            bool watching_{true};
            bool running_{};
            bool stop_requested_{};
            std::optional<std::filesystem::path> pending_{};
            bool suspend_requested_{};
            std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
            std::thread watcher_{};
        };
    }

} // namespace sogen
