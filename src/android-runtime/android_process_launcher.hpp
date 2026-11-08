#pragma once

#include <host_process.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sogen::android
{
    // A raw subprocess of the service has no Binder, so it could not show windows. The app starts each child process
    // as a service process of its own instead, and reports back here when it started and when it exited.
    class android_process_launcher final : public host_process_launcher
    {
      public:
        // Asks the app to start a child process with the managed-process arguments; it answers through started().
        using start_function = std::function<void(uint64_t request, const std::vector<std::string>& arguments)>;
        using kill_function = std::function<void(uint64_t request)>;

        android_process_launcher(start_function start, kill_function kill);

        host_process_launch_result launch(const std::vector<std::string>& command) override;

        // A process id of 0 or less means the app could not start it; -1 that every process slot is in use.
        void started(uint64_t request, int32_t process_id);
        void exited(uint64_t request, int32_t exit_status);

        // Ends every child process that is still starting or running, and makes later launches fail.
        void cancel_launches();

      private:
        struct child_state
        {
            std::optional<int32_t> process_id{};
            std::optional<int32_t> exit_status{};
            bool kill_sent{};
        };

        class child_process;

        start_function start_{};
        kill_function kill_{};

        std::mutex mutex_{};
        std::condition_variable changed_{};
        std::unordered_map<uint64_t, std::shared_ptr<child_state>> children_{};
        uint64_t next_request_{1};
        bool launches_cancelled_{};

        void kill_child(uint64_t request, child_state& state);
        std::optional<int> run_child(uint64_t request, const std::shared_ptr<child_state>& state);
    };

    // Lets a process manager use a launcher that its owner keeps alive longer than the manager.
    class process_launcher_reference final : public host_process_launcher
    {
      public:
        explicit process_launcher_reference(host_process_launcher& launcher)
            : launcher_(launcher)
        {
        }

        host_process_launch_result launch(const std::vector<std::string>& command) override
        {
            return this->launcher_.launch(command);
        }

      private:
        host_process_launcher& launcher_;
    };
}
