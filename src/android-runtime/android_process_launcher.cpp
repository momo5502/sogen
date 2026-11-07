#include "std_include.hpp"
#include "android_process_launcher.hpp"

#include <atomic>
#include <chrono>

namespace sogen::android
{
    namespace
    {
        // Binding a service process and starting its emulator library takes a while on a slow device.
        constexpr auto start_timeout = std::chrono::seconds(30);
        constexpr auto kill_poll_interval = std::chrono::milliseconds(50);
        constexpr int32_t no_free_slot = -1;
    }

    class android_process_launcher::child_process final : public host_process
    {
      public:
        child_process(android_process_launcher& launcher, const uint64_t request, std::shared_ptr<child_state> state)
            : launcher_(launcher),
              request_(request),
              state_(std::move(state))
        {
        }

        uint32_t id() const override
        {
            return static_cast<uint32_t>(*this->state_->process_id);
        }

        std::optional<int> run(const std::atomic_bool& kill_requested) override
        {
            return this->launcher_.run_child(this->request_, this->state_, kill_requested);
        }

      private:
        android_process_launcher& launcher_;
        uint64_t request_{};
        std::shared_ptr<child_state> state_{};
    };

    android_process_launcher::android_process_launcher(start_function start, kill_function kill)
        : start_(std::move(start)),
          kill_(std::move(kill))
    {
    }

    host_process_launch_result android_process_launcher::launch(const std::vector<std::string>& command)
    {
        auto state = std::make_shared<child_state>();
        uint64_t request{};
        {
            const std::scoped_lock lock(this->mutex_);
            request = this->next_request_++;
            this->children_.emplace(request, state);
        }

        // The executable is this library's; the service process runs it already.
        const std::vector<std::string> arguments(command.empty() ? command.begin() : command.begin() + 1, command.end());
        this->start_(request, arguments);

        std::unique_lock lock(this->mutex_);
        this->changed_.wait_for(lock, start_timeout, [&] { return state->process_id.has_value(); });
        const auto process_id = state->process_id.value_or(0);
        if (process_id <= 0)
        {
            const bool timed_out = !state->process_id;
            this->children_.erase(request);
            lock.unlock();
            if (timed_out)
            {
                this->kill_(request);
            }
            return {.error = process_id == no_free_slot ? process_error::resource_limit : process_error::unavailable};
        }

        return {.process = std::make_unique<child_process>(*this, request, std::move(state))};
    }

    void android_process_launcher::started(const uint64_t request, const int32_t process_id)
    {
        const std::scoped_lock lock(this->mutex_);
        if (const auto it = this->children_.find(request); it != this->children_.end())
        {
            it->second->process_id = process_id;
            this->changed_.notify_all();
        }
    }

    void android_process_launcher::exited(const uint64_t request, const int32_t exit_status)
    {
        const std::scoped_lock lock(this->mutex_);
        if (const auto it = this->children_.find(request); it != this->children_.end())
        {
            it->second->exit_status = exit_status;
            this->changed_.notify_all();
        }
    }

    std::optional<int> android_process_launcher::run_child(const uint64_t request, const std::shared_ptr<child_state>& state,
                                                           const std::atomic_bool& kill_requested)
    {
        bool kill_sent = false;
        std::unique_lock lock(this->mutex_);
        while (!state->exit_status)
        {
            if (kill_requested && !kill_sent)
            {
                kill_sent = true;
                lock.unlock();
                this->kill_(request);
                lock.lock();
                continue;
            }
            this->changed_.wait_for(lock, kill_poll_interval);
        }

        const auto exit_status = *state->exit_status;
        this->children_.erase(request);
        return exit_status;
    }
}
