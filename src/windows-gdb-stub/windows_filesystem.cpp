#include "windows_filesystem.hpp"

#include <limits>
#include <span>

#include <sys/stat.h>
#include <platform/compiler.hpp>

#undef st_atime
#undef st_mtime
#undef st_ctime

namespace sogen
{

    constexpr auto FILEIO_O_WRONLY = 0x1u;
    constexpr auto FILEIO_O_RDWR = 0x2u;
    constexpr auto FILEIO_O_APPEND = 0x8u;
    constexpr auto FILEIO_O_CREAT = 0x200u;
    constexpr auto FILEIO_O_TRUNC = 0x400u;
    constexpr auto FILEIO_O_EXCL = 0x800u;

#pragma pack(push, 1)

    struct gdb_stat
    {
        uint32_t st_dev;
        uint32_t st_ino;
        uint32_t st_mode;
        uint32_t st_nlink;
        uint32_t st_uid;
        uint32_t st_gid;
        uint32_t st_rdev;
        uint64_t st_size;
        uint64_t st_blksize;
        uint64_t st_blocks;
        uint32_t st_atime;
        uint32_t st_mtime;
        uint32_t st_ctime;
    };

    static_assert(sizeof(gdb_stat) == 0x40, "bad structure size");
#pragma pack(pop)

    static std::optional<std::u16string> flags_to_mode(uint32_t flags)
    {
        const bool read_write = (flags & FILEIO_O_RDWR) != 0;
        const bool write_only = (flags & FILEIO_O_WRONLY) != 0;
        const bool append = (flags & FILEIO_O_APPEND) != 0;
        const bool truncate = (flags & FILEIO_O_TRUNC) != 0;

        if (read_write && write_only)
        {
            return std::nullopt;
        }

        if (flags & ~(FILEIO_O_RDWR | FILEIO_O_WRONLY | FILEIO_O_APPEND | FILEIO_O_TRUNC | FILEIO_O_CREAT | FILEIO_O_EXCL))
        {
            return std::nullopt;
        }

        if (read_write)
        {
            if (append)
            {
                return u"a+b";
            }

            return truncate ? u"w+b" : u"r+b";
        }

        if (write_only)
        {
            return append ? u"ab" : u"wb";
        }

        if (truncate)
        {
            return std::nullopt;
        }

        return append ? u"a+b" : u"rb";
    }

    static std::optional<windows_path> to_guest_path(const std::string_view emulated_path)
    {
        windows_path path{std::u8string(emulated_path.begin(), emulated_path.end())};
        if (!path.is_absolute())
        {
            return std::nullopt;
        }

        return path;
    }

    uint32_t windows_filesystem::open(const std::string& file_path, const uint32_t flags, uint32_t /*mode*/)
    {
        const auto open_mode = flags_to_mode(flags);
        const auto path = to_guest_path(file_path);
        if (!open_mode || !path)
        {
            return 0;
        }

        if ((flags & FILEIO_O_CREAT) && (flags & FILEIO_O_EXCL) && this->win_emu_->file_sys.stat(*path))
        {
            return 0;
        }

        auto result = this->win_emu_->file_sys.open(*path, *open_mode);
        if (!result.file)
        {
            return 0;
        }

        opened_files.emplace(free_index, std::move(result.file));
        return free_index++;
    }

    void windows_filesystem::close(const uint32_t fd)
    {
        opened_files.erase(fd);
    }

    std::string windows_filesystem::read(const uint32_t fd, const size_t count, const uint64_t offset)
    {
        if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return {};
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        auto& file = *it->second;
        if (!file.seek(static_cast<int64_t>(offset), SEEK_SET))
        {
            return {};
        }

        std::string buffer;
        buffer.resize(count);
        buffer.resize(file.read(std::as_writable_bytes(std::span(buffer))));

        return buffer;
    }

    size_t windows_filesystem::write(const uint32_t fd, uint64_t offset, const void* data, const size_t length)
    {
        if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return 0;
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return 0;
        }

        auto& file = *it->second;
        if (!file.seek(static_cast<int64_t>(offset), SEEK_SET))
        {
            return 0;
        }

        const auto written = file.write(std::span(static_cast<const std::byte*>(data), length));
        (void)file.flush();

        return written;
    }

    std::string windows_filesystem::fstat(const uint32_t fd)
    {
        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        const auto size = it->second->size();
        if (size < 0)
        {
            return {};
        }

        gdb_stat stat_buf{};
        stat_buf.st_mode = S_IFREG;
        stat_buf.st_nlink = 1;
        stat_buf.st_size = static_cast<uint64_t>(size);

        auto* p = reinterpret_cast<char*>(&stat_buf);
        std::string buffer{p, p + sizeof(stat_buf)};
        return buffer;
    }

    int windows_filesystem::unlink(const std::string& file_path)
    {
        const auto path = to_guest_path(file_path);
        if (!path)
        {
            return -1;
        }

        return this->win_emu_->file_sys.remove(*path) == STATUS_SUCCESS ? 0 : -1;
    }

} // namespace sogen
