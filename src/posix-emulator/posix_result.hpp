#pragma once

#include <cstdint>
#include <variant>

namespace sogen
{
    enum class posix_errno
    {
        ebadf,
        einval,
        emfile,
        espipe,
    };

    using posix_result = std::variant<uint64_t, posix_errno>;
}
