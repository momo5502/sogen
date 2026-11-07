#pragma once

#include "posix_fd_table.hpp"
#include "posix_result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <variant>
#include <vector>

namespace sogen
{
    // Linux and the BSDs share the DT_* values of a directory entry's type.
    constexpr uint8_t POSIX_DT_UNKNOWN = 0;
    constexpr uint8_t POSIX_DT_FIFO = 1;
    constexpr uint8_t POSIX_DT_CHR = 2;
    constexpr uint8_t POSIX_DT_DIR = 4;
    constexpr uint8_t POSIX_DT_BLK = 6;
    constexpr uint8_t POSIX_DT_REG = 8;
    constexpr uint8_t POSIX_DT_LNK = 10;
    constexpr uint8_t POSIX_DT_SOCK = 12;

    // Entries come sorted by name after "." and "..", so listings do not depend on the host's directory order.
    const std::vector<posix_directory_entry>& posix_directory_listing(posix_fd& directory);

    using posix_directory_record_encoder =
        std::function<std::vector<std::byte>(const posix_directory_entry& entry, uint64_t next_position)>;

    struct posix_directory_records
    {
        std::vector<std::byte> data{};
        uint64_t end_position{};
        bool reached_end{};
    };

    // Packs whole records from the directory position until the next one would not fit; the caller moves the position
    // to end_position once the records reached the guest.
    std::variant<posix_directory_records, posix_errno> posix_read_directory(posix_fd& directory, uint64_t buffer_size,
                                                                            const posix_directory_record_encoder& encode);
}
