#pragma once

#include "process_manager.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sogen
{
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

        virtual void kill() = 0;

        virtual std::optional<int> run() = 0;
    };

    struct host_process_launch_result
    {
        std::unique_ptr<host_process> process{};
        process_error error{process_error::none};
    };

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

    std::unique_ptr<host_process_launcher> create_subprocess_launcher();
}
