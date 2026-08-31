#include "std_include.hpp"

#include "macos_flag_decoders.hpp"

#include "macos_guest_reader.hpp"

#include "../macos_platform.hpp"

#include <array>
#include <cstdio>

namespace sogen
{
    namespace
    {
        struct macos_enum_value
        {
            uint64_t value{};
            std::string_view name{};
        };

        std::string format_enum(const uint64_t value, const std::span<const macos_enum_value> values)
        {
            for (const auto& entry : values)
            {
                if (entry.value == value)
                {
                    return std::string(entry.name);
                }
            }

            return format_hex(value);
        }

        // Named against the emulator's own constants rather than fresh literals or a host header: these
        // decide what the guest is told, so a decoder that disagreed with them would describe a call the
        // emulator did not make.
        constexpr std::array open_access_modes{
            macos_enum_value{.value = macos_open::MACOS_O_RDONLY, .name = "O_RDONLY"},
            macos_enum_value{.value = macos_open::MACOS_O_WRONLY, .name = "O_WRONLY"},
            macos_enum_value{.value = macos_open::MACOS_O_RDWR, .name = "O_RDWR"},
        };

        constexpr std::array open_bits{
            macos_flag_bit{.mask = macos_open::MACOS_O_NONBLOCK, .name = "O_NONBLOCK"},
            macos_flag_bit{.mask = macos_open::MACOS_O_APPEND, .name = "O_APPEND"},
            macos_flag_bit{.mask = macos_open::MACOS_O_NOFOLLOW, .name = "O_NOFOLLOW"},
            macos_flag_bit{.mask = macos_open::MACOS_O_CREAT, .name = "O_CREAT"},
            macos_flag_bit{.mask = macos_open::MACOS_O_TRUNC, .name = "O_TRUNC"},
            macos_flag_bit{.mask = macos_open::MACOS_O_EXCL, .name = "O_EXCL"},
            macos_flag_bit{.mask = macos_open::MACOS_O_DIRECTORY, .name = "O_DIRECTORY"},
            macos_flag_bit{.mask = macos_open::MACOS_O_SYMLINK, .name = "O_SYMLINK"},
            macos_flag_bit{.mask = macos_open::MACOS_O_CLOEXEC, .name = "O_CLOEXEC"},
        };

        constexpr std::array protection_bits{
            macos_flag_bit{.mask = macos_mmap::MACOS_PROT_READ, .name = "PROT_READ"},
            macos_flag_bit{.mask = macos_mmap::MACOS_PROT_WRITE, .name = "PROT_WRITE"},
            macos_flag_bit{.mask = macos_mmap::MACOS_PROT_EXEC, .name = "PROT_EXEC"},
        };

        constexpr std::array map_bits{
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_SHARED, .name = "MAP_SHARED"},
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_PRIVATE, .name = "MAP_PRIVATE"},
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_FIXED, .name = "MAP_FIXED"},
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_NORESERVE, .name = "MAP_NORESERVE"},
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_JIT, .name = "MAP_JIT"},
            macos_flag_bit{.mask = macos_mmap::MACOS_MAP_ANON, .name = "MAP_ANON"},
        };

        constexpr std::array madvise_values{
            macos_enum_value{.value = macos_mmap::MACOS_MADV_NORMAL, .name = "MADV_NORMAL"},
            macos_enum_value{.value = macos_mmap::MACOS_MADV_DONTNEED, .name = "MADV_DONTNEED"},
            macos_enum_value{.value = macos_mmap::MACOS_MADV_FREE, .name = "MADV_FREE"},
            macos_enum_value{.value = macos_mmap::MACOS_MADV_FREE_REUSABLE, .name = "MADV_FREE_REUSABLE"},
            macos_enum_value{.value = macos_mmap::MACOS_MADV_FREE_REUSE, .name = "MADV_FREE_REUSE"},
            macos_enum_value{.value = macos_mmap::MACOS_MADV_ZERO, .name = "MADV_ZERO"},
        };

        constexpr std::array fcntl_values{
            macos_enum_value{.value = macos_fcntl::MACOS_F_DUPFD, .name = "F_DUPFD"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_GETFD, .name = "F_GETFD"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_SETFD, .name = "F_SETFD"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_GETFL, .name = "F_GETFL"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_SETFL, .name = "F_SETFL"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_GETPATH, .name = "F_GETPATH"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_DUPFD_CLOEXEC, .name = "F_DUPFD_CLOEXEC"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_ADDFILESIGS_RETURN, .name = "F_ADDFILESIGS_RETURN"},
            macos_enum_value{.value = macos_fcntl::MACOS_F_GETPATH_NOFIRMLINK, .name = "F_GETPATH_NOFIRMLINK"},
        };

        constexpr std::array seek_values{
            macos_enum_value{.value = macos_fcntl::MACOS_SEEK_SET, .name = "SEEK_SET"},
            macos_enum_value{.value = macos_fcntl::MACOS_SEEK_CUR, .name = "SEEK_CUR"},
            macos_enum_value{.value = macos_fcntl::MACOS_SEEK_END, .name = "SEEK_END"},
        };
    }

    std::string format_flag_bits(const uint64_t value, const std::span<const macos_flag_bit> bits)
    {
        std::string result{};
        auto remaining = value;

        for (const auto& bit : bits)
        {
            if ((remaining & bit.mask) != bit.mask)
            {
                continue;
            }

            remaining &= ~bit.mask;

            if (!result.empty())
            {
                result.push_back('|');
            }

            result += bit.name;
        }

        if (remaining != 0)
        {
            if (!result.empty())
            {
                result.push_back('|');
            }

            result += format_hex(remaining);
        }

        return result;
    }

    std::string format_open_flags(const uint64_t value)
    {
        constexpr uint64_t access_mask = macos_open::MACOS_O_ACCMODE;

        auto result = format_enum(value & access_mask, open_access_modes);
        const auto rest = format_flag_bits(value & ~access_mask, open_bits);

        if (!rest.empty())
        {
            result.push_back('|');
            result += rest;
        }

        return result;
    }

    std::string format_mmap_protection(const uint64_t value)
    {
        if (value == 0)
        {
            return "PROT_NONE";
        }

        return format_flag_bits(value, protection_bits);
    }

    std::string format_mmap_flags(const uint64_t value)
    {
        if (value == 0)
        {
            return "0";
        }

        return format_flag_bits(value, map_bits);
    }

    std::string format_madvise_advice(const uint64_t value)
    {
        return format_enum(value, madvise_values);
    }

    std::string format_fcntl_command(const uint64_t value)
    {
        return format_enum(value, fcntl_values);
    }

    std::string format_seek_whence(const uint64_t value)
    {
        return format_enum(value, seek_values);
    }

    std::string format_file_mode(const uint64_t value)
    {
        if (value == 0)
        {
            return "0";
        }

        std::array<char, 32> buffer{};
        const auto count = std::snprintf(buffer.data(), buffer.size(), "0%llo", static_cast<unsigned long long>(value));
        if (count <= 0)
        {
            return "0";
        }

        return {buffer.data(), static_cast<size_t>(count)};
    }

    std::string_view macos_errno_name(const int64_t error)
    {
        switch (error)
        {
        case macos_errno::MACOS_EPERM:
            return "EPERM";
        case macos_errno::MACOS_ENOENT:
            return "ENOENT";
        case macos_errno::MACOS_ESRCH:
            return "ESRCH";
        case macos_errno::MACOS_EINTR:
            return "EINTR";
        case macos_errno::MACOS_EIO:
            return "EIO";
        case macos_errno::MACOS_ENXIO:
            return "ENXIO";
        case macos_errno::MACOS_E2BIG:
            return "E2BIG";
        case macos_errno::MACOS_ENOEXEC:
            return "ENOEXEC";
        case macos_errno::MACOS_EBADF:
            return "EBADF";
        case macos_errno::MACOS_ECHILD:
            return "ECHILD";
        case macos_errno::MACOS_EDEADLK:
            return "EDEADLK";
        case macos_errno::MACOS_ENOMEM:
            return "ENOMEM";
        case macos_errno::MACOS_EACCES:
            return "EACCES";
        case macos_errno::MACOS_EFAULT:
            return "EFAULT";
        case macos_errno::MACOS_EBUSY:
            return "EBUSY";
        case macos_errno::MACOS_EEXIST:
            return "EEXIST";
        case macos_errno::MACOS_EXDEV:
            return "EXDEV";
        case macos_errno::MACOS_ENODEV:
            return "ENODEV";
        case macos_errno::MACOS_ENOTDIR:
            return "ENOTDIR";
        case macos_errno::MACOS_EISDIR:
            return "EISDIR";
        case macos_errno::MACOS_EINVAL:
            return "EINVAL";
        case macos_errno::MACOS_ENFILE:
            return "ENFILE";
        case macos_errno::MACOS_EMFILE:
            return "EMFILE";
        case macos_errno::MACOS_ENOTTY:
            return "ENOTTY";
        case macos_errno::MACOS_EFBIG:
            return "EFBIG";
        case macos_errno::MACOS_ENOSPC:
            return "ENOSPC";
        case macos_errno::MACOS_ESPIPE:
            return "ESPIPE";
        case macos_errno::MACOS_EROFS:
            return "EROFS";
        case macos_errno::MACOS_EMLINK:
            return "EMLINK";
        case macos_errno::MACOS_EPIPE:
            return "EPIPE";
        case macos_errno::MACOS_EDOM:
            return "EDOM";
        case macos_errno::MACOS_ERANGE:
            return "ERANGE";
        case macos_errno::MACOS_EAGAIN:
            return "EAGAIN";
        case macos_errno::MACOS_EINPROGRESS:
            return "EINPROGRESS";
        case macos_errno::MACOS_ENOTSOCK:
            return "ENOTSOCK";
        case macos_errno::MACOS_ENOTSUP:
            return "ENOTSUP";
        case macos_errno::MACOS_EAFNOSUPPORT:
            return "EAFNOSUPPORT";
        case macos_errno::MACOS_EADDRINUSE:
            return "EADDRINUSE";
        case macos_errno::MACOS_ETIMEDOUT:
            return "ETIMEDOUT";
        case macos_errno::MACOS_ECONNREFUSED:
            return "ECONNREFUSED";
        case macos_errno::MACOS_ELOOP:
            return "ELOOP";
        case macos_errno::MACOS_ENAMETOOLONG:
            return "ENAMETOOLONG";
        case macos_errno::MACOS_ENOTEMPTY:
            return "ENOTEMPTY";
        case macos_errno::MACOS_ENOSYS:
            return "ENOSYS";
        case macos_errno::MACOS_EOVERFLOW:
            return "EOVERFLOW";
        case macos_errno::MACOS_EOPNOTSUPP:
            return "EOPNOTSUPP";
        default:
            return {};
        }
    }
}
