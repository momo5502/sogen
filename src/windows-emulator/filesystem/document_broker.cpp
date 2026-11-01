#include "document_broker.hpp"
#include "document_mount.hpp"
#include "layered_provider.hpp"

#include <array>
#include <limits>
#include <cerrno>
#include <cstring>
#include <random>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <utils/serialization.hpp>

namespace sogen
{

    namespace
    {
        enum class broker_request : uint8_t
        {
            list = 1,
            open = 2,
        };

        constexpr size_t max_listing_size = 512 * 1024 * 1024;
        constexpr size_t max_token_size = 256;
        constexpr size_t max_document_id_size = 64 * 1024;

        bool send_all(const int fd, const void* data, size_t size)
        {
            const auto* bytes = static_cast<const std::byte*>(data);
            while (size > 0)
            {
                const auto sent = ::send(fd, bytes, size, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR)
                {
                    continue;
                }

                if (sent <= 0)
                {
                    return false;
                }

                bytes += sent;
                size -= static_cast<size_t>(sent);
            }

            return true;
        }

        bool receive_all(const int fd, void* data, size_t size)
        {
            auto* bytes = static_cast<std::byte*>(data);
            while (size > 0)
            {
                const auto received = ::recv(fd, bytes, size, 0);
                if (received < 0 && errno == EINTR)
                {
                    continue;
                }

                if (received <= 0)
                {
                    return false;
                }

                bytes += received;
                size -= static_cast<size_t>(received);
            }

            return true;
        }

        template <typename T>
        bool send_value(const int fd, const T& value)
        {
            return send_all(fd, &value, sizeof(value));
        }

        template <typename T>
        std::optional<T> receive_value(const int fd)
        {
            T value{};
            if (!receive_all(fd, &value, sizeof(value)))
            {
                return std::nullopt;
            }

            return value;
        }

        bool send_bytes(const int fd, const std::span<const std::byte> data)
        {
            return send_value(fd, static_cast<uint64_t>(data.size())) && send_all(fd, data.data(), data.size());
        }

        std::optional<std::vector<std::byte>> receive_bytes(const int fd, const size_t max_size)
        {
            const auto size = receive_value<uint64_t>(fd);
            if (!size || *size > max_size)
            {
                return std::nullopt;
            }

            std::vector<std::byte> data(static_cast<size_t>(*size));
            if (!receive_all(fd, data.data(), data.size()))
            {
                return std::nullopt;
            }

            return data;
        }

        bool send_descriptor(const int socket, const int fd)
        {
            constexpr uint8_t status = 1;
            iovec payload{.iov_base = const_cast<uint8_t*>(&status), .iov_len = sizeof(status)};

            alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
            msghdr message{};
            message.msg_iov = &payload;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();

            auto* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));

            while (true)
            {
                const auto sent = ::sendmsg(socket, &message, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR)
                {
                    continue;
                }

                return sent == sizeof(status);
            }
        }

        std::optional<unique_fd> receive_descriptor(const int socket)
        {
            uint8_t status{};
            iovec payload{.iov_base = &status, .iov_len = sizeof(status)};

            alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
            msghdr message{};
            message.msg_iov = &payload;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();

            ssize_t received{};
            do
            {
                received = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
            } while (received < 0 && errno == EINTR);

            if (received != sizeof(status))
            {
                return std::nullopt;
            }

            unique_fd fd{};
            for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
            {
                if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS && header->cmsg_len == CMSG_LEN(sizeof(int)))
                {
                    int value{};
                    std::memcpy(&value, CMSG_DATA(header), sizeof(value));
                    fd = unique_fd{value};
                }
            }

            if (status != 1 || !fd)
            {
                return unique_fd{};
            }

            return fd;
        }

        sockaddr_un make_address(const std::string_view name, socklen_t& length)
        {
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (name.size() + 1 > sizeof(address.sun_path))
            {
                throw std::runtime_error("Document broker name is too long");
            }

            // A leading NUL places the socket in the abstract namespace, which needs no writable directory.
            std::memcpy(address.sun_path + 1, name.data(), name.size());
            length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
            return address;
        }

        std::string random_hex(const size_t bytes)
        {
            constexpr std::string_view digits = "0123456789abcdef";
            std::random_device device{};
            std::string result{};
            result.reserve(bytes * 2);
            for (size_t i = 0; i < bytes; ++i)
            {
                const auto value = static_cast<uint8_t>(device());
                result.push_back(digits[value >> 4]);
                result.push_back(digits[value & 0xF]);
            }

            return result;
        }

        bool tokens_equal(const std::string_view a, const std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }

            uint8_t difference = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                difference |= static_cast<uint8_t>(a[i] ^ b[i]);
            }

            return difference == 0;
        }

        bool peer_is_same_user(const int fd)
        {
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 && credentials.uid == getuid();
        }

        std::optional<int64_t> regular_file_size(const int fd)
        {
            struct stat file_stat{};
            if (fstat(fd, &file_stat) != 0 || !S_ISREG(file_stat.st_mode))
            {
                return std::nullopt;
            }

            return static_cast<int64_t>(file_stat.st_size);
        }

        class brokered_document_source final : public document_source
        {
          public:
            explicit brokered_document_source(unique_fd socket)
                : socket_(std::move(socket))
            {
            }

            std::unique_ptr<i_file> open(const std::string& document_id) override
            {
                std::optional<unique_fd> fd{};
                {
                    const std::lock_guard lock{this->mutex_};
                    if (!send_value(this->socket_.get(), broker_request::open) ||
                        !send_bytes(this->socket_.get(), std::as_bytes(std::span(document_id))))
                    {
                        return nullptr;
                    }

                    fd = receive_descriptor(this->socket_.get());
                }

                if (!fd || !*fd)
                {
                    return nullptr;
                }

                const auto size = regular_file_size(fd->get());
                if (!size)
                {
                    return nullptr;
                }

                return std::make_unique<descriptor_file>(std::make_shared<const unique_fd>(std::move(*fd)), *size);
            }

          private:
            unique_fd socket_{};
            std::mutex mutex_{};
        };
    }

    unique_fd::unique_fd(const int fd)
        : fd_(fd)
    {
    }

    unique_fd::~unique_fd()
    {
        if (this->fd_ >= 0)
        {
            ::close(this->fd_);
        }
    }

    unique_fd::unique_fd(unique_fd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {
    }

    unique_fd& unique_fd::operator=(unique_fd&& other) noexcept
    {
        if (this != &other)
        {
            if (this->fd_ >= 0)
            {
                ::close(this->fd_);
            }

            this->fd_ = std::exchange(other.fd_, -1);
        }

        return *this;
    }

    descriptor_file::descriptor_file(std::shared_ptr<const unique_fd> fd, const int64_t size)
        : fd_(std::move(fd)),
          size_(size)
    {
    }

    size_t descriptor_file::read(const std::span<std::byte> buffer)
    {
        size_t total = 0;
        while (total < buffer.size() && this->position_ < this->size_)
        {
            const auto result = ::pread(this->fd_->get(), buffer.data() + total, buffer.size() - total, this->position_);
            if (result < 0 && errno == EINTR)
            {
                continue;
            }

            if (result <= 0)
            {
                break;
            }

            total += static_cast<size_t>(result);
            this->position_ += result;
        }

        return total;
    }

    size_t descriptor_file::write(const std::span<const std::byte> /*buffer*/)
    {
        return 0;
    }

    bool descriptor_file::flush()
    {
        return true;
    }

    bool descriptor_file::seek(const int64_t offset, const int origin)
    {
        int64_t base{};
        switch (origin)
        {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = this->position_;
            break;
        case SEEK_END:
            base = this->size_;
            break;
        default:
            return false;
        }

        if (offset < -base || offset > std::numeric_limits<int64_t>::max() - base)
        {
            return false;
        }

        this->position_ = base + offset;
        return true;
    }

    int64_t descriptor_file::tell()
    {
        return this->position_;
    }

    int64_t descriptor_file::size()
    {
        return this->size_;
    }

    bool descriptor_file::resize(const uint64_t /*size*/)
    {
        return false;
    }

    std::optional<struct compat_stat> descriptor_file::stat()
    {
        struct compat_stat file_stat{};
        if (!compat_fstat(this->fd_->get(), &file_stat))
        {
            return std::nullopt;
        }

        return file_stat;
    }

    descriptor_document_source::descriptor_document_source(opener open_document, const size_t capacity)
        : open_document_(std::move(open_document)),
          capacity_(capacity)
    {
    }

    std::shared_ptr<const unique_fd> descriptor_document_source::open_descriptor(const std::string& document_id)
    {
        const std::lock_guard lock{this->mutex_};

        for (auto it = this->descriptors_.begin(); it != this->descriptors_.end(); ++it)
        {
            if (it->document_id == document_id)
            {
                this->descriptors_.splice(this->descriptors_.begin(), this->descriptors_, it);
                return it->fd;
            }
        }

        auto fd = this->open_document_(document_id);
        if (!fd || !regular_file_size(fd.get()))
        {
            return nullptr;
        }

        auto shared = std::make_shared<const unique_fd>(std::move(fd));
        this->descriptors_.push_front(cached_descriptor{.document_id = document_id, .fd = shared});
        if (this->descriptors_.size() > this->capacity_)
        {
            this->descriptors_.pop_back();
        }

        return shared;
    }

    std::unique_ptr<i_file> descriptor_document_source::open(const std::string& document_id)
    {
        auto fd = this->open_descriptor(document_id);
        if (!fd)
        {
            return nullptr;
        }

        const auto size = regular_file_size(fd->get());
        if (!size)
        {
            return nullptr;
        }

        return std::make_unique<descriptor_file>(std::move(fd), *size);
    }

    document_broker::document_broker(std::vector<document_entry> entries, std::shared_ptr<descriptor_document_source> source)
        : entries_(std::move(entries)),
          source_(std::move(source)),
          name_("sogen-documents-" + random_hex(16)),
          token_(random_hex(32))
    {
        for (size_t i = 0; i < this->entries_.size(); ++i)
        {
            if (!this->entries_[i].is_directory)
            {
                this->files_.emplace(this->entries_[i].id, i);
            }
        }

        this->listener_ = unique_fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (!this->listener_)
        {
            throw std::runtime_error("Failed to create the document broker socket: " + std::string{strerror(errno)});
        }

        socklen_t length{};
        const auto address = make_address(this->name_, length);
        if (::bind(this->listener_.get(), reinterpret_cast<const sockaddr*>(&address), length) != 0 ||
            ::listen(this->listener_.get(), 16) != 0)
        {
            throw std::runtime_error("Failed to listen on the document broker socket: " + std::string{strerror(errno)});
        }

        this->accept_thread_ = std::thread([this] { this->accept_clients(); });
    }

    document_broker::~document_broker()
    {
        {
            const std::lock_guard lock{this->mutex_};
            this->stopping_ = true;
            for (const auto& client : this->clients_)
            {
                ::shutdown(client.socket.get(), SHUT_RDWR);
            }
        }

        ::shutdown(this->listener_.get(), SHUT_RDWR);
        this->accept_thread_.join();

        for (auto& client : this->clients_)
        {
            client.thread.join();
        }
    }

    void document_broker::accept_clients()
    {
        while (true)
        {
            unique_fd client{::accept4(this->listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
            if (!client)
            {
                if (errno == EINTR || errno == ECONNABORTED)
                {
                    continue;
                }

                return;
            }

            if (!peer_is_same_user(client.get()))
            {
                continue;
            }

            this->join_finished_clients();

            const std::lock_guard lock{this->mutex_};
            if (this->stopping_)
            {
                return;
            }

            auto& entry = this->clients_.emplace_back();
            entry.socket = std::move(client);
            entry.thread = std::thread([this, &entry] {
                this->serve(entry.socket.get());
                ::shutdown(entry.socket.get(), SHUT_RDWR);
                entry.finished = true;
            });
        }
    }

    void document_broker::join_finished_clients()
    {
        std::list<client> finished{};
        {
            const std::lock_guard lock{this->mutex_};
            for (auto it = this->clients_.begin(); it != this->clients_.end();)
            {
                const auto current = it++;
                if (current->finished)
                {
                    finished.splice(finished.end(), this->clients_, current);
                }
            }
        }

        for (auto& client : finished)
        {
            client.thread.join();
        }
    }

    void document_broker::serve(const int socket)
    {
        const auto token = receive_bytes(socket, max_token_size);
        if (!token || !tokens_equal(std::string_view{reinterpret_cast<const char*>(token->data()), token->size()}, this->token_))
        {
            return;
        }

        while (true)
        {
            const auto request_value = receive_value<uint8_t>(socket);
            if (!request_value)
            {
                break;
            }

            const auto request = static_cast<broker_request>(*request_value);
            if (request == broker_request::list)
            {
                utils::buffer_serializer buffer{};
                buffer.write_vector(this->entries_);
                if (!send_bytes(socket, buffer.get_buffer()))
                {
                    break;
                }

                continue;
            }

            if (request != broker_request::open)
            {
                break;
            }

            const auto id_data = receive_bytes(socket, max_document_id_size);
            if (!id_data)
            {
                break;
            }

            const std::string document_id{reinterpret_cast<const char*>(id_data->data()), id_data->size()};
            const auto fd = this->files_.contains(document_id) ? this->source_->open_descriptor(document_id) : nullptr;
            const auto sent = fd ? send_descriptor(socket, fd->get()) : send_value(socket, uint8_t{0});
            if (!sent)
            {
                break;
            }
        }
    }

    brokered_document_tree connect_document_broker(const std::string& name, const std::string& token)
    {
        unique_fd socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (!socket)
        {
            throw std::runtime_error("Failed to create a document broker socket: " + std::string{strerror(errno)});
        }

        socklen_t length{};
        const auto address = make_address(name, length);
        if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), length) != 0)
        {
            throw std::runtime_error("Failed to connect to the document broker: " + std::string{strerror(errno)});
        }

        if (!send_bytes(socket.get(), std::as_bytes(std::span(token))) || !send_value(socket.get(), broker_request::list))
        {
            throw std::runtime_error("Failed to request the document tree from the broker");
        }

        const auto data = receive_bytes(socket.get(), max_listing_size);
        if (!data)
        {
            throw std::runtime_error("The document broker refused the connection");
        }

        utils::buffer_deserializer buffer{*data};
        brokered_document_tree tree{};
        buffer.read_vector(tree.entries);
        tree.source = std::make_shared<brokered_document_source>(std::move(socket));
        return tree;
    }

    std::unique_ptr<mount_provider> connect_document_mount(const document_mount& mount)
    {
        auto tree = connect_document_broker(mount.broker, mount.token);
        auto lower = std::make_unique<document_tree_provider>(std::move(tree.entries), std::move(tree.source));
        return std::make_unique<layered_provider>(std::move(lower), mount.upper_directory, mount.state_directory);
    }

} // namespace sogen
