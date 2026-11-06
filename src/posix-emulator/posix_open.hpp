#pragma once

#include "posix_fd_table.hpp"
#include "posix_file_system.hpp"
#include "posix_result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sogen
{
    enum class posix_access_mode
    {
        read_only,
        write_only,
        read_write,
    };

    // Linux and XNU share O_ACCMODE and its values; the fourth value is invalid on both.
    std::optional<posix_access_mode> posix_access_mode_from_flags(int flags);

    struct posix_open_request
    {
        std::string guest_path{};
        posix_access_mode access{posix_access_mode::read_only};
        bool create{};
        bool exclusive{};
        bool truncate{};
        bool append{};
        bool directory{};
        bool close_on_exec{};
        uint32_t creation_mode{};

        // Descriptors keep the guest's own open flags, which F_GETFL reports back.
        int guest_flags{};
    };

    posix_result posix_open(posix_fd_table& fds, const posix_file_system& file_system, const posix_open_request& request);
}
