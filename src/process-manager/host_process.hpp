#pragma once

#include "process_manager.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sogen
{
    // The host process that runs one managed guest process.
    class host_process
    {
      public:
        host_process() = default;
        virtual ~host_process() = default;

        host_process(const host_process&) = delete;
        host_process& operator=(const host_process&) = delete;
        host_process(host_process&&) = delete;
        host_process& operator=(host_process&&) = delete;

        virtual uint32_t id() const = 0;

        // Ends the process. Safe to call from any thread and more than once, while run() is running or after it returned.
        virtual void kill() = 0;

        // Runs until the process exits, forwarding what it writes. Returns its exit status, or nothing when it could not
        // be watched.
        virtual std::optional<int> run() = 0;
    };

    struct host_process_launch_result
    {
        std::unique_ptr<host_process> process{};
        process_error error{process_error::none};
    };

    // Starts the host processes of managed guest processes, so that a platform can run them as its own kind of process.
    class host_process_launcher
    {
      public:
        host_process_launcher() = default;
        virtual ~host_process_launcher() = default;

        host_process_launcher(const host_process_launcher&) = delete;
        host_process_launcher& operator=(const host_process_launcher&) = delete;
        host_process_launcher(host_process_launcher&&) = delete;
        host_process_launcher& operator=(host_process_launcher&&) = delete;

        virtual host_process_launch_result launch(const std::vector<std::string>& command) = 0;
    };

    // Runs each command as a subprocess of this process.
    std::unique_ptr<host_process_launcher> create_subprocess_launcher();
}
