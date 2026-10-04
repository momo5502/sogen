#pragma once

#include "process_manager.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sogen
{
    struct in_process_process
    {
        virtual ~in_process_process() = default;

        virtual process_exit run() = 0;
        virtual void terminate(uint64_t exit_code) = 0;
    };

    class in_process_process_manager final : public process_manager
    {
      public:
        using process_factory = std::function<std::unique_ptr<in_process_process>(process_create_request)>;

        explicit in_process_process_manager(process_factory factory);
        ~in_process_process_manager() override;

        in_process_process_manager(const in_process_process_manager&) = delete;
        in_process_process_manager& operator=(const in_process_process_manager&) = delete;
        in_process_process_manager(in_process_process_manager&&) = delete;
        in_process_process_manager& operator=(in_process_process_manager&&) = delete;

        process_create_result create_process(process_create_request request) override;
        process_error terminate_process(managed_process process, uint64_t exit_code) override;
        process_exit_status_result exit_status(managed_process process) const override;

      private:
        struct process_entry;

        std::shared_ptr<process_entry> find_process(managed_process process) const;

        process_factory factory_{};
        mutable std::mutex mutex_{};
        std::unordered_map<uint64_t, std::shared_ptr<process_entry>> processes_{};
        uint64_t next_process_{1};
        bool stopping_{};
    };
}
