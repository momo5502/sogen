#include "host_pipe.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include "nt_handle.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sogen::utils
{
#ifdef _WIN32
    namespace
    {
        pipe_io_result io_error(const DWORD error, const DWORD transferred = 0)
        {
            switch (error)
            {
            case ERROR_SUCCESS:
            case ERROR_MORE_DATA:
                return {pipe_io_status::success, transferred};
            case ERROR_IO_PENDING:
            case ERROR_IO_INCOMPLETE:
                return {};
            case ERROR_BROKEN_PIPE:
            case ERROR_PIPE_NOT_CONNECTED:
            case ERROR_NO_DATA:
                return {pipe_io_status::broken, 0};
            case ERROR_OPERATION_ABORTED:
                return {pipe_io_status::cancelled, 0};
            default:
                return {pipe_io_status::failed, 0};
            }
        }

        void require_handle(const HANDLE handle)
        {
            if (!handle || handle == INVALID_HANDLE_VALUE)
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Creating host pipe");
            }
        }
    }

    struct host_pipe::implementation
    {
        explicit implementation(const HANDLE handle)
            : handle(handle)
        {
            require_handle(handle);
        }

        nt::handle<nt::invalid_handle> handle;
    };

    struct host_pipe_operation::implementation
    {
        std::shared_ptr<host_pipe::implementation> pipe;
        std::string buffer;
        nt::handle<> event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED overlapped{};
        pipe_io_result result{};

        implementation(std::shared_ptr<host_pipe::implementation> pipe, std::string buffer)
            : pipe(std::move(pipe)),
              buffer(std::move(buffer))
        {
            require_handle(this->event);
            this->overlapped.hEvent = this->event;
        }

        ~implementation()
        {
            this->cancel();
        }

        void cancel()
        {
            if (this->result.status == pipe_io_status::pending)
            {
                CancelIoEx(this->pipe->handle, &this->overlapped);
                DWORD transferred{};
                const auto success = GetOverlappedResult(this->pipe->handle, &this->overlapped, &transferred, TRUE);
                this->result = io_error(success ? ERROR_SUCCESS : GetLastError(), transferred);
            }
        }
    };

    std::pair<std::shared_ptr<host_pipe>, std::shared_ptr<host_pipe>> host_pipe::create_pair(const bool message_type, const uint32_t quota)
    {
        static std::atomic_uint64_t next_pipe{};
        const auto name = L"\\\\.\\pipe\\sogen-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(next_pipe++);
        const auto mode = (message_type ? PIPE_TYPE_MESSAGE : PIPE_TYPE_BYTE) | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        auto server = adopt(reinterpret_cast<uint64_t>(CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, mode, 1, quota, quota, 0, nullptr)));
        auto client = adopt(reinterpret_cast<uint64_t>(
            CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)));
        nt::handle<> event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        require_handle(event);
        OVERLAPPED connected{};
        connected.hEvent = event;
        const auto server_handle = reinterpret_cast<HANDLE>(server->native_handle());
        const auto connect_result = ConnectNamedPipe(server_handle, &connected);
        const auto connect_error = connect_result ? ERROR_SUCCESS : GetLastError();
        if (connect_error != ERROR_SUCCESS && connect_error != ERROR_IO_PENDING && connect_error != ERROR_PIPE_CONNECTED)
        {
            throw std::system_error(static_cast<int>(connect_error), std::system_category(), "Connecting host pipe");
        }
        if (connect_error == ERROR_IO_PENDING)
        {
            DWORD transferred{};
            if (!GetOverlappedResult(server_handle, &connected, &transferred, TRUE))
            {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Connecting host pipe");
            }
        }
        return {std::move(server), std::move(client)};
    }

    std::shared_ptr<host_pipe> host_pipe::adopt(const uint64_t native_handle)
    {
        return std::make_shared<host_pipe>(std::make_shared<implementation>(reinterpret_cast<HANDLE>(native_handle)));
    }

    uint64_t host_pipe::native_handle() const
    {
        return reinterpret_cast<uint64_t>(static_cast<HANDLE>(this->implementation_->handle));
    }

    uint64_t host_pipe::duplicate_into(const uint32_t process_id) const
    {
        nt::handle<> process{OpenProcess(PROCESS_DUP_HANDLE, FALSE, process_id)};
        require_handle(process);
        HANDLE duplicated{};
        if (!DuplicateHandle(GetCurrentProcess(), this->implementation_->handle, process, &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Duplicating host pipe");
        }
        return reinterpret_cast<uint64_t>(duplicated);
    }

    std::shared_ptr<host_pipe> host_pipe::duplicate() const
    {
        return adopt(this->duplicate_into(GetCurrentProcessId()));
    }

    std::unique_ptr<host_pipe_operation> host_pipe::read(const uint32_t size)
    {
        auto operation = std::make_unique<host_pipe_operation::implementation>(this->implementation_, std::string(size, '\0'));
        DWORD transferred{};
        const auto success = ReadFile(this->implementation_->handle, operation->buffer.data(), size, &transferred, &operation->overlapped);
        operation->result = io_error(success ? ERROR_SUCCESS : GetLastError(), transferred);
        return std::make_unique<host_pipe_operation>(std::move(operation));
    }

    std::unique_ptr<host_pipe_operation> host_pipe::write(const std::string_view data)
    {
        auto operation = std::make_unique<host_pipe_operation::implementation>(this->implementation_, std::string(data));
        DWORD transferred{};
        const auto success = WriteFile(this->implementation_->handle, operation->buffer.data(),
                                       static_cast<DWORD>(operation->buffer.size()), &transferred, &operation->overlapped);
        operation->result = io_error(success ? ERROR_SUCCESS : GetLastError(), transferred);
        return std::make_unique<host_pipe_operation>(std::move(operation));
    }

    pipe_io_result host_pipe::peek(std::string& data, const uint32_t capacity, uint32_t& available) const
    {
        data.resize(capacity);
        DWORD copied{};
        DWORD total{};
        const auto success = PeekNamedPipe(this->implementation_->handle, data.data(), capacity, &copied, &total, nullptr);
        data.resize(copied);
        available = total;
        return io_error(success ? ERROR_SUCCESS : GetLastError(), copied);
    }

    pipe_io_result host_pipe_operation::check_completion()
    {
        auto& operation = *this->implementation_;
        if (operation.result.status == pipe_io_status::pending)
        {
            DWORD transferred{};
            const auto success = GetOverlappedResult(operation.pipe->handle, &operation.overlapped, &transferred, FALSE);
            operation.result = io_error(success ? ERROR_SUCCESS : GetLastError(), transferred);
        }
        return operation.result;
    }
#else
    struct host_pipe::implementation
    {
        explicit implementation(const int descriptor)
            : descriptor(descriptor)
        {
            if (descriptor < 0)
            {
                throw std::system_error(errno, std::generic_category(), "Creating host pipe");
            }
        }

        ~implementation()
        {
            close(this->descriptor);
        }

        int descriptor;
    };

    struct host_pipe_operation::implementation
    {
        std::shared_ptr<host_pipe::implementation> pipe;
        std::string buffer;
        bool reading{};
        pipe_io_result result{};

        void cancel()
        {
            if (this->result.status == pipe_io_status::pending)
            {
                this->result.status = pipe_io_status::cancelled;
            }
        }
    };

    std::pair<std::shared_ptr<host_pipe>, std::shared_ptr<host_pipe>> host_pipe::create_pair(const bool message_type, uint32_t)
    {
        if (message_type)
        {
            throw std::system_error(std::make_error_code(std::errc::not_supported), "Message-mode host pipes");
        }
        int descriptors[2]{};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "Creating host pipe");
        }
        auto server = adopt(static_cast<uint64_t>(descriptors[0]));
        auto client = adopt(static_cast<uint64_t>(descriptors[1]));
        for (const auto descriptor : descriptors)
        {
            if (fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0 || fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
            {
                throw std::system_error(errno, std::generic_category(), "Configuring host pipe");
            }
#ifdef SO_NOSIGPIPE
            const int enabled = 1;
            if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
            {
                throw std::system_error(errno, std::generic_category(), "Configuring host pipe");
            }
#endif
        }
        return {std::move(server), std::move(client)};
    }

    std::shared_ptr<host_pipe> host_pipe::adopt(const uint64_t native_handle)
    {
        return std::make_shared<host_pipe>(std::make_shared<implementation>(static_cast<int>(native_handle)));
    }

    uint64_t host_pipe::native_handle() const
    {
        return static_cast<uint64_t>(this->implementation_->descriptor);
    }

    uint64_t host_pipe::duplicate_into(uint32_t) const
    {
        throw std::system_error(std::make_error_code(std::errc::not_supported), "Cross-process pipe descriptor transfer");
    }

    std::shared_ptr<host_pipe> host_pipe::duplicate() const
    {
        return adopt(static_cast<uint64_t>(fcntl(this->implementation_->descriptor, F_DUPFD_CLOEXEC, 0)));
    }

    std::unique_ptr<host_pipe_operation> host_pipe::read(const uint32_t size)
    {
        auto operation = std::make_unique<host_pipe_operation::implementation>();
        operation->pipe = this->implementation_;
        operation->buffer.resize(size);
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
        data.resize(std::max(capacity, 65536u));
        const auto result = recv(this->implementation_->descriptor, data.data(), data.size(), MSG_PEEK);
        available = result > 0 ? static_cast<uint32_t>(result) : 0;
        data.resize(std::min(capacity, available));
        if (result == 0)
        {
            return {pipe_io_status::broken, 0};
        }
        return {result < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? pipe_io_status::failed : pipe_io_status::success,
                static_cast<uint32_t>(data.size())};
    }

    pipe_io_result host_pipe_operation::check_completion()
    {
        auto& operation = *this->implementation_;
        if (operation.result.status != pipe_io_status::pending)
        {
            return operation.result;
        }
        if (operation.buffer.empty())
        {
            operation.result.status = pipe_io_status::success;
            return operation.result;
        }
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto count = operation.reading ? recv(operation.pipe->descriptor, operation.buffer.data(), operation.buffer.size(), 0)
                                             : send(operation.pipe->descriptor, operation.buffer.data() + operation.result.transferred,
                                                    operation.buffer.size() - operation.result.transferred, send_flags);
        if (count > 0)
        {
            operation.result.transferred += static_cast<uint32_t>(count);
            if (operation.reading || operation.result.transferred == operation.buffer.size())
            {
                operation.result.status = pipe_io_status::success;
            }
        }
        else if (count == 0 || errno == EPIPE || errno == ECONNRESET)
        {
            operation.result.status = pipe_io_status::broken;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            operation.result.status = pipe_io_status::failed;
        }
        return operation.result;
    }
#endif

    host_pipe::host_pipe(std::shared_ptr<implementation> implementation)
        : implementation_(std::move(implementation))
    {
    }

    host_pipe::~host_pipe() = default;

    host_pipe_operation::host_pipe_operation(std::unique_ptr<implementation> implementation)
        : implementation_(std::move(implementation))
    {
    }

    host_pipe_operation::~host_pipe_operation() = default;

    void host_pipe_operation::cancel()
    {
        this->implementation_->cancel();
    }

    std::string_view host_pipe_operation::data() const
    {
        return this->implementation_->buffer;
    }
}
