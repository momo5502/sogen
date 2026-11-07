#include "posix_directory.hpp"
#include "posix_file_system.hpp"
#include "posix_stat.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace sogen
{
    namespace
    {
        uint8_t directory_entry_type(const std::filesystem::file_type type)
        {
            switch (type)
            {
            case std::filesystem::file_type::regular:
                return POSIX_DT_REG;
            case std::filesystem::file_type::directory:
                return POSIX_DT_DIR;
            case std::filesystem::file_type::symlink:
                return POSIX_DT_LNK;
            case std::filesystem::file_type::block:
                return POSIX_DT_BLK;
            case std::filesystem::file_type::character:
                return POSIX_DT_CHR;
            case std::filesystem::file_type::fifo:
                return POSIX_DT_FIFO;
            case std::filesystem::file_type::socket:
                return POSIX_DT_SOCK;
            default:
                return POSIX_DT_UNKNOWN;
            }
        }

        uint64_t host_inode(const std::filesystem::path& path)
        {
            const auto status = posix_stat_host_path(path, false);
            const auto* file_status = std::get_if<posix_file_status>(&status);
            return file_status ? file_status->inode : 0;
        }

        std::vector<posix_directory_entry> list_directory(const std::filesystem::path& directory)
        {
            std::vector<posix_directory_entry> entries{};

            std::error_code ec{};
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                std::error_code type_error{};
                entries.push_back({
                    .inode = host_inode(entry.path()),
                    .type = directory_entry_type(entry.symlink_status(type_error).type()),
                    .name = utf8_path_string(entry.path().filename()),
                });
            }

            std::ranges::sort(entries, {}, &posix_directory_entry::name);

            const auto parent = directory.has_parent_path() ? directory.parent_path() : directory;
            entries.insert(entries.begin(), {
                                                {.inode = host_inode(directory), .type = POSIX_DT_DIR, .name = "."},
                                                {.inode = host_inode(parent), .type = POSIX_DT_DIR, .name = ".."},
                                            });
            return entries;
        }
    }

    // Reading from the start lists the directory again, so rewinddir sees entries created since it was opened.
    const std::vector<posix_directory_entry>& posix_directory_listing(posix_fd& directory)
    {
        if (!directory.directory_listing || directory.directory_position == 0)
        {
            directory.directory_listing = list_directory(path_from_utf8(directory.host_path));
        }

        return *directory.directory_listing;
    }

    std::variant<posix_directory_records, posix_errno> posix_read_directory(posix_fd& directory, const uint64_t buffer_size,
                                                                            const posix_directory_record_encoder& encode)
    {
        const auto& entries = posix_directory_listing(directory);

        posix_directory_records records{};
        auto position = directory.directory_position;
        for (; position < entries.size(); ++position)
        {
            const auto record = encode(entries.at(static_cast<size_t>(position)), position + 1);
            if (records.data.size() + record.size() > buffer_size)
            {
                break;
            }

            records.data.insert(records.data.end(), record.begin(), record.end());
        }

        if (records.data.empty() && position < entries.size())
        {
            return posix_errno::einval;
        }

        records.end_position = position;
        records.reached_end = position >= entries.size();
        return records;
    }
}
