#include "posix_open.hpp"
#include "posix_host_error.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sogen
{
    namespace
    {
        constexpr int ACCESS_MODE_MASK = 0x3;

        bool modifies_file(const posix_open_request& request)
        {
            return request.access != posix_access_mode::read_only || request.create || request.truncate;
        }

        // fdopen's mode only has to agree with the descriptor: open has already created or truncated the file.
        const char* stream_mode(const posix_open_request& request)
        {
            switch (request.access)
            {
            case posix_access_mode::write_only:
                return request.append ? "ab" : "wb";
            case posix_access_mode::read_write:
                return request.append ? "a+b" : "r+b";
            default:
                return "rb";
            }
        }

#if defined(_WIN32)
        posix_errno posix_errno_from_windows(const DWORD error)
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                return posix_errno::enoent;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
                return posix_errno::eacces;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS:
                return posix_errno::eexist;
            case ERROR_TOO_MANY_OPEN_FILES:
                return posix_errno::emfile;
            case ERROR_DISK_FULL:
                return posix_errno::enospc;
            default:
                return posix_errno::eio;
            }
        }

        DWORD creation_disposition(const posix_open_request& request)
        {
            if (request.create && request.exclusive)
            {
                return CREATE_NEW;
            }

            if (request.create)
            {
                return request.truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
            }

            return request.truncate ? TRUNCATE_EXISTING : OPEN_EXISTING;
        }

        std::variant<int, posix_errno> open_host_descriptor(const std::filesystem::path& host_path, const posix_open_request& request)
        {
            DWORD desired_access = GENERIC_READ;
            int crt_flags = _O_BINARY | _O_RDONLY;
            if (request.access == posix_access_mode::write_only)
            {
                desired_access = GENERIC_WRITE;
                crt_flags = _O_BINARY | _O_WRONLY;
            }
            else if (request.access == posix_access_mode::read_write)
            {
                desired_access = GENERIC_READ | GENERIC_WRITE;
                crt_flags = _O_BINARY | _O_RDWR;
            }

            if (request.append)
            {
                crt_flags |= _O_APPEND;
            }

            auto* const handle = CreateFileW(host_path.c_str(), desired_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, creation_disposition(request), FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return posix_errno_from_windows(GetLastError());
            }

            const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), crt_flags);
            if (descriptor < 0)
            {
                CloseHandle(handle);
                return posix_errno::emfile;
            }

            return descriptor;
        }

        FILE* open_stream(const int descriptor, const char* mode)
        {
            return _fdopen(descriptor, mode);
        }

        void close_host_descriptor(const int descriptor)
        {
            _close(descriptor);
        }
#else
        int host_open_flags(const posix_open_request& request)
        {
            int flags = O_RDONLY;
            if (request.access == posix_access_mode::write_only)
            {
                flags = O_WRONLY;
            }
            else if (request.access == posix_access_mode::read_write)
            {
                flags = O_RDWR;
            }

            if (request.create)
            {
                flags |= O_CREAT;
            }
            if (request.exclusive)
            {
                flags |= O_EXCL;
            }
            if (request.truncate)
            {
                flags |= O_TRUNC;
            }
            if (request.append)
            {
                flags |= O_APPEND;
            }

            return flags;
        }

        std::variant<int, posix_errno> open_host_descriptor(const std::filesystem::path& host_path, const posix_open_request& request)
        {
            constexpr uint32_t permission_bits = 07777;

            const auto descriptor = ::open(host_path.c_str(), host_open_flags(request), request.creation_mode & permission_bits);
            if (descriptor < 0)
            {
                return posix_errno_from_host(errno);
            }

            return descriptor;
        }

        FILE* open_stream(const int descriptor, const char* mode)
        {
            return ::fdopen(descriptor, mode);
        }

        void close_host_descriptor(const int descriptor)
        {
            ::close(descriptor);
        }
#endif

        std::variant<FILE*, posix_errno> open_host_stream(const std::filesystem::path& host_path, const posix_open_request& request)
        {
            const auto descriptor = open_host_descriptor(host_path, request);
            if (const auto* error = std::get_if<posix_errno>(&descriptor))
            {
                return *error;
            }

            auto* stream = open_stream(std::get<int>(descriptor), stream_mode(request));
            if (stream == nullptr)
            {
                close_host_descriptor(std::get<int>(descriptor));
                return posix_errno::emfile;
            }

            std::setvbuf(stream, nullptr, _IONBF, 0);
            return stream;
        }

        bool has_non_directory_ancestor(const std::filesystem::path& host_path)
        {
            for (auto ancestor = host_path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path())
            {
                std::error_code ec{};
                const auto status = std::filesystem::status(ancestor, ec);
                if (std::filesystem::exists(status))
                {
                    return !std::filesystem::is_directory(status);
                }

                if (ancestor == ancestor.parent_path())
                {
                    break;
                }
            }

            return false;
        }

        // Windows reports a path below a regular file as missing, so ENOTDIR is derived from the path when the host
        // gives no more specific reason.
        posix_errno unresolved_path_error(const std::filesystem::path& host_path, const std::error_code& ec)
        {
            const auto condition = ec.default_error_condition();
            if (ec && condition.category() == std::generic_category() && condition.value() != ENOENT)
            {
                return posix_errno_from_host(condition.value());
            }

            return has_non_directory_ancestor(host_path) ? posix_errno::enotdir : posix_errno::enoent;
        }

        posix_fd make_descriptor(const fd_type type, const std::filesystem::path& host_path, const posix_open_request& request,
                                 const bool read_only_mapping)
        {
            posix_fd file{};
            file.type = type;
            file.host_path = utf8_path_string(host_path);
            file.guest_path = request.guest_path;
            file.flags = request.guest_flags;
            file.read_only_mapping = read_only_mapping;
            return file;
        }
    }

    std::optional<posix_access_mode> posix_access_mode_from_flags(const int flags)
    {
        switch (flags & ACCESS_MODE_MASK)
        {
        case 0:
            return posix_access_mode::read_only;
        case 1:
            return posix_access_mode::write_only;
        case 2:
            return posix_access_mode::read_write;
        default:
            return std::nullopt;
        }
    }

    posix_result posix_open(posix_fd_table& fds, const posix_file_system& file_system, const posix_open_request& request)
    {
        const auto host_path = file_system.translate(request.guest_path);
        const auto read_only_mapping = file_system.is_read_only_guest_path(request.guest_path);

        std::error_code ec{};
        const auto status = std::filesystem::status(host_path, ec);
        const auto exists = std::filesystem::exists(status);

        if (exists && request.create && request.exclusive)
        {
            return posix_errno::eexist;
        }

        if (std::filesystem::is_directory(status))
        {
            if (request.access != posix_access_mode::read_only || request.truncate)
            {
                return posix_errno::eisdir;
            }

            const auto fd = fds.allocate(make_descriptor(fd_type::directory, host_path, request, read_only_mapping), request.close_on_exec);
            return static_cast<uint64_t>(fd);
        }

        if (!exists && (request.directory || !request.create))
        {
            return unresolved_path_error(host_path, ec);
        }

        if (request.directory)
        {
            return posix_errno::enotdir;
        }

        if (read_only_mapping && modifies_file(request))
        {
            return posix_errno::erofs;
        }

        const auto stream = open_host_stream(host_path, request);
        if (const auto* error = std::get_if<posix_errno>(&stream))
        {
            return *error;
        }

        auto file = make_descriptor(fd_type::file, host_path, request, read_only_mapping);
        file.handle = std::get<FILE*>(stream);
        return static_cast<uint64_t>(fds.allocate(std::move(file), request.close_on_exec));
    }
}
