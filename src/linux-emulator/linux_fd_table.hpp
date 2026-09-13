#pragma once

#include <guest/guest_fd_table.hpp>

namespace sogen
{
    using linux_memory_fd = guest_memory_fd;
    using linux_socket_state = guest_socket_state;
    using linux_fd = guest_fd;
    using linux_fd_table = guest_fd_table;
}
