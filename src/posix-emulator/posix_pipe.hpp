#pragma once

#include "posix_fd_table.hpp"
#include "posix_result.hpp"

#include <cstdint>
#include <optional>
#include <variant>

namespace sogen
{
    struct posix_pipe
    {
        int read_fd{};
        int write_fd{};
    };

    // nonblock_flag is the guest's O_NONBLOCK for a non-blocking pipe and 0 otherwise, since descriptors keep the
    // guest's open flags.
    std::variant<posix_pipe, posix_errno> posix_create_pipe(posix_fd_table& fds, int nonblock_flag, bool close_on_exec);

    std::optional<uint64_t> posix_pipe_queued_bytes(const posix_fd& read_end);

    // A pipe end blocks in the host, so its host descriptor follows the guest's non-blocking state.
    void posix_set_nonblocking(posix_fd& file, int nonblock_flag, bool nonblocking);
}
