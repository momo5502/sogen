#pragma once

#include <cstdint>
#include <variant>

namespace sogen
{
    enum class posix_errno
    {
        eagain,
        ebadf,
        efault,
        eintr,
        einval,
        eio,
        eisdir,
        emfile,
        enospc,
        enotconn,
        epipe,
        espipe,
    };

    using posix_result = std::variant<uint64_t, posix_errno>;

    // Like the kernels, a transfer that fails after moving some data reports the partial count.
    inline posix_result posix_partial_transfer(const uint64_t transferred, const posix_errno error)
    {
        if (transferred != 0)
        {
            return transferred;
        }

        return error;
    }
}
