#pragma once

#include "posix_fd_table.hpp"
#include "posix_memory_manager.hpp"
#include "posix_result.hpp"

#include <memory_permission.hpp>

#include <cstdint>
#include <optional>

namespace sogen
{
    // Linux and XNU share the PROT_* values.
    constexpr uint64_t POSIX_PROT_READ = 0x1;
    constexpr uint64_t POSIX_PROT_WRITE = 0x2;
    constexpr uint64_t POSIX_PROT_EXEC = 0x4;

    memory_permission posix_memory_permission(uint64_t protection);

    enum class posix_map_placement
    {
        hint,
        fixed,
        fixed_noreplace,
    };

    struct posix_map_request
    {
        uint64_t address{};
        uint64_t length{};
        uint64_t protection{};
        posix_map_placement placement{posix_map_placement::hint};
        std::optional<int> fd{};
        uint64_t offset{};
    };

    // A length that wraps when rounded up to a page fails mmap and mprotect with ENOMEM on Linux and EINVAL on XNU, and
    // munmap with EINVAL on both. A rounded length larger than the address space fails mmap with ENOMEM on both.
    posix_result posix_mmap(posix_memory_manager& memory, const posix_fd_table& fds, const posix_map_request& request,
                            posix_errno oversized_length_error);
    posix_result posix_munmap(posix_memory_manager& memory, uint64_t address, uint64_t length);
    posix_result posix_mprotect(posix_memory_manager& memory, uint64_t address, uint64_t length, uint64_t protection,
                                posix_errno oversized_length_error);
}
