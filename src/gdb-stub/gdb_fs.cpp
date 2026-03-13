#include "gdb_fs.hpp"

#include <limits>

#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <platform/compiler.hpp>

#undef st_atime
#undef st_mtime
#undef st_ctime

namespace gdb_stub
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

    static std::string flags_to_mode(uint32_t flags)
    {
        std::string result;

        bool plus_mode = false;
        bool exclusive = false;

        if (flags & FILEIO_O_EXCL)
        {
            exclusive = true;
            flags &= ~FILEIO_O_EXCL;
        }

        if (flags & FILEIO_O_RDWR)
        {
            plus_mode = true;
            flags &= ~FILEIO_O_RDWR;
        }

        if (flags & FILEIO_O_APPEND)
        {
            result.push_back('a');
            flags &= ~(FILEIO_O_APPEND | FILEIO_O_CREAT | FILEIO_O_WRONLY);
        }
        else if ((flags & FILEIO_O_WRONLY) || (flags & FILEIO_O_TRUNC))
        {
            result.push_back('w');
            flags &= ~(FILEIO_O_WRONLY | FILEIO_O_TRUNC | FILEIO_O_CREAT);
        }
        else
        {
            result.push_back('r');
        }

        if (flags)
        {
            return {};
        }

        if (plus_mode)
        {
            result.push_back('+');
        }

        result.push_back('b');

        if (exclusive)
        {
            result.push_back('x');
        }

        return result;
    }

    unsigned gdb_fs::open(const std::string& file_path, const uint32_t flags, uint32_t /*mode*/)
    {
        const auto open_mode = flags_to_mode(flags);
        if (open_mode.empty())
        {
            return 0;
        }

        auto* fp = std::fopen(file_path.c_str(), open_mode.c_str());
        if (!fp)
        {
            return 0;
        }

        opened_files.insert({free_index, auto_file_t(fp)});
        return free_index++;
    }

    void gdb_fs::close(const unsigned fd)
    {
        auto it = opened_files.find(fd);
        if (it != opened_files.end())
        {
            opened_files.erase(it);
        }
    }

    std::string gdb_fs::read(const unsigned fd, const size_t count, const uint64_t offset)
    {
        if (count > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return {};
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        auto* fp = it->second.get();
        if (_fseeki64(fp, static_cast<int64_t>(offset), SEEK_SET))
        {
            return {};
        }

        std::string buffer;
        buffer.resize(count);

        const auto n = std::fread(buffer.data(), 1, count, fp);
        buffer.resize(n);

        return buffer;
    }

    size_t gdb_fs::write(const unsigned fd, uint64_t offset, const void* data, const size_t length)
    {
        if (length > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return 0;
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return 0;
        }

        auto* fp = it->second.get();
        if (_fseeki64(fp, static_cast<int64_t>(offset), SEEK_SET))
        {
            return 0;
        }

        return std::fwrite(data, 1, length, fp);
    }

    std::string gdb_fs::fstat(const unsigned fd)
    {
        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        auto* fp = it->second.get();
        const auto saved_position = _ftelli64(fp);

        if (_fseeki64(fp, 0, SEEK_END))
        {
            return {};
        }

        const auto size = _ftelli64(fp);
        _fseeki64(fp, saved_position, SEEK_SET);

        gdb_stat stat_buf{};
        stat_buf.st_mode = S_IFREG;
        stat_buf.st_nlink = 1;
        stat_buf.st_size = static_cast<uint64_t>(size);

        auto* p = reinterpret_cast<char*>(&stat_buf);
        std::string buffer{p, p + sizeof(stat_buf)};
        return buffer;
    }

    int gdb_fs::unlink(const std::string& file_path)
    {
        (void)this; // prevent static warning
        return ::unlink(file_path.c_str());
    }
}
