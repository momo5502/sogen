#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace sogen::utils
{
    class host_pipe_transfer;

    enum class pipe_io_status
    {
        success,
        pending,
        broken,
        cancelled,
        failed,
    };

    struct pipe_io_result
    {
        pipe_io_status status{pipe_io_status::pending};
        uint32_t transferred{};
    };

    class host_pipe_operation
    {
      public:
        struct implementation;

        explicit host_pipe_operation(std::unique_ptr<implementation> implementation);
        ~host_pipe_operation();
        host_pipe_operation(const host_pipe_operation&) = delete;
        host_pipe_operation& operator=(const host_pipe_operation&) = delete;

        pipe_io_result check_completion();
        void cancel();
        std::string_view data() const;

      private:
        std::unique_ptr<implementation> implementation_;
    };

    class host_pipe
    {
      public:
        struct implementation;

        static std::pair<std::shared_ptr<host_pipe>, std::shared_ptr<host_pipe>> create_pair(bool message_type, uint32_t quota);
        static std::shared_ptr<host_pipe> connect(uint16_t port, const std::string& token);
        explicit host_pipe(std::shared_ptr<implementation> implementation);
        ~host_pipe();
        host_pipe(const host_pipe&) = delete;
        host_pipe& operator=(const host_pipe&) = delete;

        std::shared_ptr<host_pipe> duplicate() const;
        host_pipe_transfer export_endpoint() const;
        std::unique_ptr<host_pipe_operation> read(uint32_t size);
        std::unique_ptr<host_pipe_operation> write(std::string_view data);
        pipe_io_result peek(std::string& data, uint32_t capacity, uint32_t& available) const;

      private:
        std::shared_ptr<implementation> implementation_;
    };

    class host_pipe_transfer
    {
      public:
        struct implementation;

        explicit host_pipe_transfer(std::shared_ptr<host_pipe::implementation> endpoint);
        ~host_pipe_transfer();
        host_pipe_transfer(host_pipe_transfer&&) noexcept;
        host_pipe_transfer& operator=(host_pipe_transfer&&) noexcept;
        host_pipe_transfer(const host_pipe_transfer&) = delete;
        host_pipe_transfer& operator=(const host_pipe_transfer&) = delete;

        uint16_t port() const;
        const std::string& token() const;

      private:
        std::unique_ptr<implementation> implementation_;
    };
}
