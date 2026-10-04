#pragma once

#include "process_manager.hpp"

#include <network/tcp_client_socket.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
        managed_process_connection(managed_process_connection&& other) noexcept;
        managed_process_connection& operator=(managed_process_connection&&) = delete;
        ~managed_process_connection();

        const process_create_request& request() const;
        bool wait_for_resume(managed_process_target& target);
        bool notify_exit(uint64_t exit_code);
        void disconnect();

      private:
        bool handle_messages(managed_process_target& target, bool wait_for_resume);

        network::tcp_client_socket socket_{};
        process_create_request request_{};
        std::mutex send_mutex_{};
        std::thread control_thread_{};
    };

    managed_process_connection connect_managed_process(uint16_t port, const std::string& token);

    class subprocess_process_manager final : public process_manager
    {
      public:
        using argument_factory = std::function<std::vector<std::string>(uint16_t, const std::string&)>;

        subprocess_process_manager(std::filesystem::path executable, argument_factory arguments);
        ~subprocess_process_manager() override;

        subprocess_process_manager(const subprocess_process_manager&) = delete;
        subprocess_process_manager& operator=(const subprocess_process_manager&) = delete;
        subprocess_process_manager(subprocess_process_manager&&) = delete;
        subprocess_process_manager& operator=(subprocess_process_manager&&) = delete;

        process_create_result create_process(process_create_request request) override;
        process_error resume_process(managed_process process) override;
        process_error terminate_process(managed_process process, uint64_t exit_code) override;
        process_exit_status_result exit_status(managed_process process) const override;
        process_memory_result allocate_memory(managed_process process, uint64_t address, uint64_t size, memory_permission permission,
                                              bool reserve, bool commit) override;
        process_error free_memory(managed_process process, uint64_t address, uint64_t size, bool release) override;
        process_memory_result protect_memory(managed_process process, uint64_t address, uint64_t size,
                                             memory_permission permission) override;
        process_memory_read_result read_memory(managed_process process, uint64_t address, uint64_t size) const override;
        process_memory_result write_memory(managed_process process, uint64_t address, std::span<const uint8_t> data) override;

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
