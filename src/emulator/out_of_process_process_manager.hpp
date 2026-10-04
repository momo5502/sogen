#pragma once

#include "process_manager.hpp"

#include <network/tcp_client_socket.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sogen
{
    class managed_process_connection
    {
      public:
        managed_process_connection() = default;
        managed_process_connection(network::tcp_client_socket socket, process_create_request request);

        managed_process_connection(const managed_process_connection&) = delete;
        managed_process_connection& operator=(const managed_process_connection&) = delete;
        managed_process_connection(managed_process_connection&&) noexcept = default;
        managed_process_connection& operator=(managed_process_connection&&) noexcept = default;

        const process_create_request& request() const;
        bool notify_started();
        bool notify_exit(uint64_t exit_code);

      private:
        network::tcp_client_socket socket_{};
        process_create_request request_{};
    };

    managed_process_connection connect_managed_process(uint16_t port, const std::string& token);

    class out_of_process_process_manager final : public process_manager
    {
      public:
        using argument_factory = std::function<std::vector<std::string>(uint16_t, const std::string&)>;

        out_of_process_process_manager(std::filesystem::path executable, argument_factory arguments);
        ~out_of_process_process_manager() override;

        out_of_process_process_manager(const out_of_process_process_manager&) = delete;
        out_of_process_process_manager& operator=(const out_of_process_process_manager&) = delete;
        out_of_process_process_manager(out_of_process_process_manager&&) = delete;
        out_of_process_process_manager& operator=(out_of_process_process_manager&&) = delete;

        process_create_result create_process(process_create_request request) override;
        process_error terminate_process(managed_process process, uint64_t exit_code) override;
        process_exit_status_result exit_status(managed_process process) const override;

      private:
        struct process_entry;

        std::shared_ptr<process_entry> find_process(managed_process process) const;

        std::filesystem::path executable_{};
        argument_factory arguments_{};
        mutable std::mutex mutex_{};
        std::unordered_map<uint64_t, std::shared_ptr<process_entry>> processes_{};
        uint64_t next_process_{1};
        bool stopping_{};
    };
}
