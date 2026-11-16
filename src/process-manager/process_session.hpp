#pragma once

#include "process_manager.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

namespace sogen
{
    class process_session;

    class session_process
    {
      public:
        const process_identity& identity() const;
        process_exit_status_result exit_status() const;

      private:
        friend class process_session;
        process_identity identity_{};
        mutable std::mutex mutex_{};
        process_exit_status_result status_{};

        void update_status(process_exit_status_result status);
    };

    struct process_session_lookup_result
    {
        std::shared_ptr<session_process> process{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    struct process_session_reservation_result
    {
        uint32_t process_id{};
        uint32_t thread_id{};
        process_error error{process_error::none};

        explicit operator bool() const
        {
            return this->error == process_error::none;
        }
    };

    class process_session
    {
      public:
        using clock_source = std::function<std::chrono::steady_clock::time_point()>;
        static constexpr auto watch_lifetime = std::chrono::seconds(1);

        explicit process_session(process_session_endpoint remote = {}, clock_source now = std::chrono::steady_clock::now,
                                 std::chrono::milliseconds request_timeout = std::chrono::seconds(5));
        ~process_session();

        process_session(const process_session&) = delete;
        process_session& operator=(const process_session&) = delete;
        process_session(process_session&&) = delete;
        process_session& operator=(process_session&&) = delete;

        process_session_endpoint endpoint() const;
        bool owns_registry() const;
        process_session_reservation_result reserve_process(uint32_t process_id = {}, uint32_t initial_thread_id = {});
        guest_id_result allocate_thread_id(uint32_t process_id, uint32_t requested_id = {});
        process_error discard_process(uint32_t process_id);
        process_exit_status_result exit_status(const std::shared_ptr<session_process>& process) const;
        process_exit_status_result poll_exit_status(const std::shared_ptr<session_process>& process) const;
        process_session_lookup_result register_process(process_identity identity, uint32_t initial_thread_id = {});
        process_session_lookup_result publish_process(process_identity identity);
        process_session_lookup_result lookup(uint32_t process_id) const;
        process_error mark_exit(uint32_t process_id, const process_exit& exit);

      private:
        struct implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
