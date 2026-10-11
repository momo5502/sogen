#include "host_pipe.hpp"

#include "../network/tcp_server_socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

namespace sogen::utils
{
    namespace
    {
        constexpr uint32_t maximum_payload_size = 64U << 20;
        constexpr auto response_timeout = std::chrono::seconds(5);
        constexpr size_t token_size = 32;

        enum class pipe_request : uint32_t
        {
            read,
            write,
            peek,
            close,
        };

        std::string make_token()
        {
            std::random_device random;
            constexpr std::string_view digits = "0123456789abcdef";
            std::string token;
            token.reserve(token_size);
            for (size_t i = 0; i < token_size / 8; ++i)
            {
                auto value = random();
                for (size_t j = 0; j < 8; ++j)
                {
                    token.push_back(digits[value & 15]);
                    value >>= 4;
                }
            }
            return token;
        }

        std::optional<std::string> receive_exact(network::tcp_client_socket& socket, const size_t size,
                                                 const std::chrono::steady_clock::time_point deadline,
                                                 const std::atomic_bool* stopping = nullptr)
        {
            std::string data;
            while (data.size() < size)
            {
                if (!socket || (stopping && *stopping) || std::chrono::steady_clock::now() >= deadline)
                {
                    return std::nullopt;
                }
                if (!socket.is_ready(true))
                {
                    socket.sleep(std::chrono::milliseconds(1), true);
                    continue;
                }
                auto part = socket.receive(size - data.size());
                if (!part)
                {
                    if (socket && (GET_SOCKET_ERROR() == SERR(EWOULDBLOCK) || GET_SOCKET_ERROR() == SERR(EINTR)))
                    {
                        continue;
                    }
                    return std::nullopt;
                }
                data.append(*part);
            }
            return data;
        }

        template <size_t N>
        std::optional<std::array<uint32_t, N>> receive_words(network::tcp_client_socket& socket,
                                                             const std::chrono::steady_clock::time_point deadline,
                                                             const std::atomic_bool* stopping = nullptr)
        {
            const auto data = receive_exact(socket, N * sizeof(uint32_t), deadline, stopping);
            if (!data)
            {
                return std::nullopt;
            }
            std::array<uint32_t, N> words{};
            std::memcpy(words.data(), data->data(), data->size());
            for (auto& word : words)
            {
                word = ntohl(word);
            }
            return words;
        }

        template <size_t N>
        bool send_words(network::tcp_client_socket& socket, std::array<uint32_t, N> words, const std::string_view data = {})
        {
            for (auto& word : words)
            {
                word = htonl(word);
            }
            std::string message(reinterpret_cast<const char*>(words.data()), sizeof(words));
            message.append(data);
            return socket.send(message);
        }
    }

    struct host_pipe::implementation
    {
        virtual ~implementation() = default;
        virtual pipe_io_result read(std::string& data, uint32_t size) = 0;
        virtual pipe_io_result write(std::string_view data) = 0;
        virtual pipe_io_result peek(std::string& data, uint32_t capacity, uint32_t& available) = 0;
    };

    namespace
    {
        struct pipe_state
        {
            std::mutex mutex;
            std::array<std::deque<char>, 2> buffers;
            std::array<bool, 2> open{true, true};
            uint32_t quota;
            bool message_type;

            pipe_state(const uint32_t quota, const bool message_type)
                : quota(quota ? quota : 4096),
                  message_type(message_type)
            {
            }
        };

        struct local_endpoint final : host_pipe::implementation
        {
            std::shared_ptr<pipe_state> state;
            size_t side;

            local_endpoint(std::shared_ptr<pipe_state> state, const size_t side)
                : state(std::move(state)),
                  side(side)
            {
            }

            ~local_endpoint() override
            {
                const std::scoped_lock lock(this->state->mutex);
                this->state->open[this->side] = false;
            }

            pipe_io_result read(std::string& data, const uint32_t size) override
            {
                const std::scoped_lock lock(this->state->mutex);
                auto& buffer = this->state->buffers[this->side];
                const auto count = std::min(static_cast<size_t>(size), buffer.size());
                data.assign(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(count));
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(count));
                if (count || !size)
                {
                    return {pipe_io_status::success, static_cast<uint32_t>(count)};
                }
                return {this->state->open[1 - this->side] ? pipe_io_status::pending : pipe_io_status::broken, 0};
            }

            pipe_io_result write(const std::string_view data) override
            {
                const std::scoped_lock lock(this->state->mutex);
                if (!this->state->open[1 - this->side])
                {
                    return {pipe_io_status::broken, 0};
                }
                auto& buffer = this->state->buffers[1 - this->side];
                const auto available = this->state->quota - std::min(static_cast<size_t>(this->state->quota), buffer.size());
                auto count = std::min(data.size(), available);
                if (this->state->message_type)
                {
                    count = buffer.empty() || data.size() <= available ? data.size() : 0;
                }
                buffer.insert(buffer.end(), data.begin(), data.begin() + static_cast<ptrdiff_t>(count));
                return {count || data.empty() ? pipe_io_status::success : pipe_io_status::pending, static_cast<uint32_t>(count)};
            }

            pipe_io_result peek(std::string& data, const uint32_t capacity, uint32_t& available) override
            {
                const std::scoped_lock lock(this->state->mutex);
                const auto& buffer = this->state->buffers[this->side];
                available = static_cast<uint32_t>(buffer.size());
                const auto count = std::min(static_cast<size_t>(capacity), buffer.size());
                data.assign(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(count));
                return {buffer.empty() && !this->state->open[1 - this->side] ? pipe_io_status::broken : pipe_io_status::success,
                        static_cast<uint32_t>(count)};
            }
        };

        struct remote_endpoint final : host_pipe::implementation
        {
            network::tcp_client_socket socket;
            std::mutex mutex;

            explicit remote_endpoint(network::tcp_client_socket socket)
                : socket(std::move(socket))
            {
            }

            ~remote_endpoint() override
            {
                try
                {
                    std::string output;
                    uint32_t available{};
                    (void)this->request(pipe_request::close, 0, {}, output, available);
                }
                catch (const std::exception&)
                {
                }
            }

            pipe_io_result request(const pipe_request operation, const uint32_t size, const std::string_view input, std::string& output,
                                   uint32_t& available)
            {
                const std::scoped_lock lock(this->mutex);
                const auto deadline = std::chrono::steady_clock::now() + response_timeout;
                if (!this->socket || !send_words(this->socket, std::array{static_cast<uint32_t>(operation), size}, input))
                {
                    this->socket.close();
                    return {pipe_io_status::broken, 0};
                }
                const auto response = receive_words<3>(this->socket, deadline);
                if (!response || (*response)[0] > static_cast<uint32_t>(pipe_io_status::failed) || (*response)[1] > size)
                {
                    this->socket.close();
                    return {pipe_io_status::broken, 0};
                }
                const pipe_io_result result{static_cast<pipe_io_status>((*response)[0]), (*response)[1]};
                available = (*response)[2];
                if (operation != pipe_request::write)
                {
                    auto data = receive_exact(this->socket, result.transferred, deadline);
                    if (!data)
                    {
                        this->socket.close();
                        return {pipe_io_status::broken, 0};
                    }
                    output = std::move(*data);
                }
                return result;
            }

            pipe_io_result read(std::string& data, const uint32_t size) override
            {
                uint32_t available{};
                return this->request(pipe_request::read, std::min(size, maximum_payload_size), {}, data, available);
            }

            pipe_io_result write(const std::string_view data) override
            {
                std::string output;
                uint32_t available{};
                const auto part = data.substr(0, maximum_payload_size);
                return this->request(pipe_request::write, static_cast<uint32_t>(part.size()), part, output, available);
            }

            pipe_io_result peek(std::string& data, const uint32_t capacity, uint32_t& available) override
            {
                return this->request(pipe_request::peek, std::min(capacity, maximum_payload_size), {}, data, available);
            }
        };
    }

    struct host_pipe_transfer::implementation
    {
        uint16_t port{};
        std::string token{make_token()};
        std::atomic_bool stopping{};
        std::thread worker;

        explicit implementation(std::shared_ptr<host_pipe::implementation> endpoint)
        {
            network::tcp_server_socket server{AF_INET};
            if (!server.bind(network::address{"127.0.0.1", 0}) || !server.set_blocking(false))
            {
                throw std::runtime_error("Binding pipe endpoint transfer");
            }
            server.listen();
            this->port = server.get_port();
            this->worker = std::thread([this, server = std::move(server), endpoint = std::move(endpoint)]() mutable {
                while (!this->stopping)
                {
                    if (!server.is_ready(true))
                    {
                        server.sleep(std::chrono::milliseconds(1), true);
                        continue;
                    }
                    auto socket = server.accept();
                    if (!socket || !socket.set_blocking(false))
                    {
                        continue;
                    }
                    const auto deadline = std::chrono::steady_clock::now() + response_timeout;
                    const auto authentication = receive_exact(socket, token_size, deadline, &this->stopping);
                    if (!authentication || *authentication != this->token)
                    {
                        continue;
                    }
                    if (send_words(socket, std::array{uint32_t{1}}))
                    {
                        server.close();
                        try
                        {
                            this->serve(socket, endpoint);
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                    break;
                }
            });
        }

        ~implementation()
        {
            this->stopping = true;
            if (this->worker.joinable())
            {
                this->worker.join();
            }
        }

        void serve(network::tcp_client_socket& socket, std::shared_ptr<host_pipe::implementation>& endpoint) const
        {
            while (!this->stopping)
            {
                if (!socket.is_ready(true))
                {
                    socket.sleep(std::chrono::milliseconds(1), true);
                    continue;
                }
                const auto deadline = std::chrono::steady_clock::now() + response_timeout;
                const auto request = receive_words<2>(socket, deadline, &this->stopping);
                if (!request || (*request)[1] > maximum_payload_size)
                {
                    return;
                }
                const auto operation = static_cast<pipe_request>((*request)[0]);
                const auto size = (*request)[1];
                std::string data;
                pipe_io_result result;
                uint32_t available{};
                switch (operation)
                {
                case pipe_request::read:
                    result = endpoint->read(data, size);
                    break;
                case pipe_request::write: {
                    const auto input = receive_exact(socket, size, deadline, &this->stopping);
                    if (!input)
                    {
                        return;
                    }
                    result = endpoint->write(*input);
                    break;
                }
                case pipe_request::peek:
                    result = endpoint->peek(data, size, available);
                    break;
                case pipe_request::close:
                    endpoint.reset();
                    (void)send_words(socket, std::array{static_cast<uint32_t>(pipe_io_status::success), uint32_t{0}, uint32_t{0}});
                    return;
                default:
                    return;
                }
                if (!send_words(socket, std::array{static_cast<uint32_t>(result.status), result.transferred, available}, data))
                {
                    return;
                }
            }
        }
    };

    struct host_pipe_operation::implementation
    {
        std::shared_ptr<host_pipe::implementation> pipe;
        std::string buffer;
        uint32_t size{};
        bool reading{};
        pipe_io_result result;
    };

    std::pair<std::shared_ptr<host_pipe>, std::shared_ptr<host_pipe>> host_pipe::create_pair(const bool message_type, const uint32_t quota)
    {
        auto state = std::make_shared<pipe_state>(quota, message_type);
        return {std::make_shared<host_pipe>(std::make_shared<local_endpoint>(state, 0)),
                std::make_shared<host_pipe>(std::make_shared<local_endpoint>(std::move(state), 1))};
    }

    std::shared_ptr<host_pipe> host_pipe::connect(const uint16_t port, const std::string& token)
    {
        network::tcp_client_socket socket{AF_INET};
        if (token.size() != token_size || !socket.connect(network::address{"127.0.0.1", port}) || !socket.set_blocking(false) ||
            !socket.send(token))
        {
            throw std::runtime_error("Connecting transferred pipe endpoint");
        }
        const auto authenticated = receive_words<1>(socket, std::chrono::steady_clock::now() + response_timeout);
        if (!authenticated || (*authenticated)[0] != 1)
        {
            throw std::runtime_error("Authenticating transferred pipe endpoint");
        }
        return std::make_shared<host_pipe>(std::make_shared<remote_endpoint>(std::move(socket)));
    }

    host_pipe::host_pipe(std::shared_ptr<implementation> implementation)
        : implementation_(std::move(implementation))
    {
    }

    host_pipe::~host_pipe() = default;

    std::shared_ptr<host_pipe> host_pipe::duplicate() const
    {
        return std::make_shared<host_pipe>(this->implementation_);
    }

    host_pipe_transfer host_pipe::export_endpoint() const
    {
        return host_pipe_transfer{this->implementation_};
    }

    std::unique_ptr<host_pipe_operation> host_pipe::read(const uint32_t size)
    {
        auto operation = std::make_unique<host_pipe_operation::implementation>();
        operation->pipe = this->implementation_;
        operation->size = size;
        operation->reading = true;
        return std::make_unique<host_pipe_operation>(std::move(operation));
    }

    std::unique_ptr<host_pipe_operation> host_pipe::write(const std::string_view data)
    {
        auto operation = std::make_unique<host_pipe_operation::implementation>();
        operation->pipe = this->implementation_;
        operation->buffer = data;
        return std::make_unique<host_pipe_operation>(std::move(operation));
    }

    pipe_io_result host_pipe::peek(std::string& data, const uint32_t capacity, uint32_t& available) const
    {
        return this->implementation_->peek(data, capacity, available);
    }

    host_pipe_transfer::host_pipe_transfer(std::shared_ptr<host_pipe::implementation> endpoint)
        : implementation_(std::make_unique<implementation>(std::move(endpoint)))
    {
    }

    host_pipe_transfer::~host_pipe_transfer() = default;
    host_pipe_transfer::host_pipe_transfer(host_pipe_transfer&&) noexcept = default;
    host_pipe_transfer& host_pipe_transfer::operator=(host_pipe_transfer&&) noexcept = default;

    uint16_t host_pipe_transfer::port() const
    {
        return this->implementation_->port;
    }

    const std::string& host_pipe_transfer::token() const
    {
        return this->implementation_->token;
    }

    host_pipe_operation::host_pipe_operation(std::unique_ptr<implementation> implementation)
        : implementation_(std::move(implementation))
    {
    }

    host_pipe_operation::~host_pipe_operation() = default;

    pipe_io_result host_pipe_operation::check_completion()
    {
        auto& operation = *this->implementation_;
        if (operation.result.status != pipe_io_status::pending)
        {
            return operation.result;
        }
        if (operation.reading)
        {
            operation.result = operation.pipe->read(operation.buffer, operation.size);
        }
        else
        {
            const auto part = operation.pipe->write(std::string_view(operation.buffer).substr(operation.result.transferred));
            operation.result.transferred += part.transferred;
            operation.result.status = part.status;
            if (part.status == pipe_io_status::success && operation.result.transferred < operation.buffer.size())
            {
                operation.result.status = pipe_io_status::pending;
            }
        }
        return operation.result;
    }

    void host_pipe_operation::cancel()
    {
        if (this->implementation_->result.status == pipe_io_status::pending)
        {
            this->implementation_->result.status = pipe_io_status::cancelled;
        }
    }

    std::string_view host_pipe_operation::data() const
    {
        return this->implementation_->buffer;
    }
}
