#include "posix_memory_syscalls.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr size_t FILE_COPY_CHUNK_SIZE = 0x10000;

        bool is_page_aligned(const uint64_t value)
        {
            return page_align_down(value) == value;
        }

        std::optional<size_t> page_rounded_length(const uint64_t length)
        {
            const auto rounded = page_align_up(length);
            if (rounded < length)
            {
                return std::nullopt;
            }

            return static_cast<size_t>(rounded);
        }

        bool is_mappable(const posix_fd& file)
        {
            return file.type == fd_type::file && !file.is_standard_stream();
        }

        // Pages past the end of the file stay zero, as the fresh mapping already is.
        void copy_file_to_memory(posix_memory_manager& memory, const posix_fd& file, const uint64_t offset, const uint64_t address,
                                 const size_t length)
        {
            std::vector<std::byte> chunk(std::min(length, FILE_COPY_CHUNK_SIZE));
            size_t copied = 0;
            while (copied < length)
            {
                const auto chunk_size = std::min(chunk.size(), length - copied);
                const auto read = file.read_at(offset + copied, std::span{chunk}.first(chunk_size));
                memory.write_memory(address + copied, chunk.data(), read);
                if (read < chunk_size)
                {
                    return;
                }

                copied += read;
            }
        }

        posix_result map_region(posix_memory_manager& memory, const posix_map_request& request, const size_t length)
        {
            switch (request.placement)
            {
            case posix_map_placement::fixed_noreplace:
                if (memory.overlaps_mapped_region(request.address, length))
                {
                    return posix_errno::eexist;
                }

                if (!memory.allocate_memory(request.address, length, memory_permission::read_write))
                {
                    return posix_errno::enomem;
                }

                return request.address;
            case posix_map_placement::fixed:
                if (!memory.replace_memory(request.address, length, memory_permission::read_write))
                {
                    return posix_errno::enomem;
                }

                return request.address;
            case posix_map_placement::hint:
                break;
            }

            const auto address = memory.allocate_memory(length, memory_permission::read_write, request.address);
            if (address == 0)
            {
                return posix_errno::enomem;
            }

            return address;
        }
    }

    memory_permission posix_memory_permission(const uint64_t protection)
    {
        auto permission = memory_permission::none;

        if (protection & POSIX_PROT_READ)
        {
            permission |= memory_permission::read;
        }

        if (protection & POSIX_PROT_WRITE)
        {
            permission |= memory_permission::write;
        }

        if (protection & POSIX_PROT_EXEC)
        {
            permission |= memory_permission::exec;
        }

        return permission;
    }

    posix_result posix_mmap(posix_memory_manager& memory, const posix_fd_table& fds, const posix_map_request& request,
                            const posix_errno oversized_length_error)
    {
        const auto is_fixed = request.placement != posix_map_placement::hint;
        if (request.length == 0 || !is_page_aligned(request.offset) ||
            (is_fixed && (request.address == 0 || !is_page_aligned(request.address))))
        {
            return posix_errno::einval;
        }

        const auto* file = request.fd ? fds.get(*request.fd) : nullptr;
        if (request.fd && !file)
        {
            return posix_errno::ebadf;
        }

        if (file && !is_mappable(*file))
        {
            return posix_errno::enodev;
        }

        const auto rounded_length = page_rounded_length(request.length);
        if (!rounded_length)
        {
            return oversized_length_error;
        }

        const auto length = *rounded_length;
        if (length > memory.get_max_mmap_end())
        {
            return posix_errno::enomem;
        }

        const auto mapped = map_region(memory, request, length);
        if (std::holds_alternative<posix_errno>(mapped))
        {
            return mapped;
        }

        const auto address = std::get<uint64_t>(mapped);
        if (file)
        {
            copy_file_to_memory(memory, *file, request.offset, address, length);
        }

        memory.protect_memory(address, length, posix_memory_permission(request.protection));
        return address;
    }

    // Unmapping a range that holds no mapping is not an error.
    posix_result posix_munmap(posix_memory_manager& memory, const uint64_t address, const uint64_t length)
    {
        const auto rounded_length = page_rounded_length(length);
        if (length == 0 || !rounded_length || !is_page_aligned(address) || address > std::numeric_limits<uint64_t>::max() - *rounded_length)
        {
            return posix_errno::einval;
        }

        memory.release_memory(address, *rounded_length);
        return uint64_t{0};
    }

    posix_result posix_mprotect(posix_memory_manager& memory, const uint64_t address, const uint64_t length, const uint64_t protection,
                                const posix_errno oversized_length_error)
    {
        if (length == 0 || !is_page_aligned(address))
        {
            return posix_errno::einval;
        }

        const auto rounded_length = page_rounded_length(length);
        if (!rounded_length)
        {
            return oversized_length_error;
        }

        if (!memory.protect_memory(address, *rounded_length, posix_memory_permission(protection)))
        {
            return posix_errno::enomem;
        }

        return uint64_t{0};
    }
}
