#pragma once

#include "posix_result.hpp"

#include <cerrno>

namespace sogen
{
    inline posix_errno posix_errno_from_host(const int host_errno)
    {
        switch (host_errno)
        {
        case EACCES:
            return posix_errno::eacces;
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return posix_errno::eagain;
        case EBADF:
            return posix_errno::ebadf;
        case EEXIST:
            return posix_errno::eexist;
        case EINTR:
            return posix_errno::eintr;
        case EINVAL:
            return posix_errno::einval;
        case EISDIR:
            return posix_errno::eisdir;
        case EMFILE:
        case ENFILE:
            return posix_errno::emfile;
        case ENOENT:
            return posix_errno::enoent;
        case ENOMEM:
            return posix_errno::enomem;
        case ENOSPC:
            return posix_errno::enospc;
        case ENOTDIR:
            return posix_errno::enotdir;
        case EPERM:
            return posix_errno::eperm;
        case EPIPE:
            return posix_errno::epipe;
        case EROFS:
            return posix_errno::erofs;
        default:
            return posix_errno::eio;
        }
    }
}
