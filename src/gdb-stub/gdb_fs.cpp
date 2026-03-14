#include "gdb_fs.hpp"

#include <ios>
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

    static std::ios_base::openmode flags_to_mode(uint32_t flags)
    {
        std::ios_base::openmode mode = std::ios::binary;

        if (flags & FILEIO_O_RDWR)
        {
            mode |= std::ios::in | std::ios::out;
            flags &= ~FILEIO_O_RDWR;

            if (flags & FILEIO_O_WRONLY)
            {
                return {};
            }
        }

        if (flags & FILEIO_O_WRONLY)
        {
            mode |= std::ios::out;
            flags &= ~FILEIO_O_WRONLY;
        }
        else
        {
            mode |= std::ios::in;
        }

        if (flags & FILEIO_O_APPEND)
        {
            mode |= std::ios::app;
            flags &= ~FILEIO_O_APPEND;
        }

        if (flags & FILEIO_O_TRUNC)
        {
            mode |= std::ios::trunc;
            flags &= ~FILEIO_O_TRUNC;
        }

        if (flags & ~(FILEIO_O_CREAT | FILEIO_O_EXCL))
        {
            return {};
        }

        return mode;
    }

    uint32_t gdb_filesystem::open(const std::string& file_path, const uint32_t flags, uint32_t /*mode*/)
    {
        const auto open_mode = flags_to_mode(flags);
        if (open_mode == 0)
        {
            return 0;
        }

        auto stream = std::fstream(file_path, open_mode);
        stream.exceptions(std::fstream::goodbit);

        opened_files.emplace(free_index, std::move(stream));
        return free_index++;
    }

    void gdb_filesystem::close(const uint32_t fd)
    {
        auto it = opened_files.find(fd);
        if (it != opened_files.end())
        {
            it->second.close();
            opened_files.erase(it);
        }
    }

    std::string gdb_filesystem::read(const uint32_t fd, const size_t count, const uint64_t offset)
    {
        if (count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()) ||
            offset > static_cast<uint64_t>(std::numeric_limits<std::fstream::off_type>::max()))
        {
            return {};
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        auto& stream = it->second;
        stream.clear();
        stream.seekg(static_cast<std::fstream::off_type>(offset), std::ios::beg);

        if (stream.fail())
        {
            return {};
        }

        std::string buffer;
        buffer.resize(count);

        stream.read(buffer.data(), static_cast<std::streamsize>(count));
        buffer.resize(static_cast<size_t>(stream.gcount()));

        return buffer;
    }

    size_t gdb_filesystem::write(const uint32_t fd, uint64_t offset, const void* data, const size_t length)
    {
        if (length > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()) ||
            offset > static_cast<uint64_t>(std::numeric_limits<std::fstream::off_type>::max()))
        {
            return 0;
        }

        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return 0;
        }

        auto& stream = it->second;
        stream.clear();
        stream.seekp(static_cast<std::fstream::off_type>(offset), std::ios::beg);

        if (stream.fail())
        {
            return 0;
        }

        const auto before = stream.tellp();
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
        const auto after = stream.tellp();

        return static_cast<size_t>(after - before);
    }

    std::string gdb_filesystem::fstat(const uint32_t fd)
    {
        const auto it = opened_files.find(fd);
        if (it == opened_files.end())
        {
            return {};
        }

        auto& stream = it->second;
        stream.clear();
        stream.seekg(0, std::fstream::end);

        if (stream.fail())
        {
            return {};
        }

        const auto end_pos = stream.tellg();

        gdb_stat stat_buf{};
        stat_buf.st_mode = S_IFREG;
        stat_buf.st_nlink = 1;
        stat_buf.st_size = static_cast<uint64_t>(end_pos);

        auto* p = reinterpret_cast<char*>(&stat_buf);
        std::string buffer{p, p + sizeof(stat_buf)};
        return buffer;
    }

    int gdb_filesystem::unlink(const std::string& file_path)
    {
        (void)this; // prevent static warning
        return ::unlink(file_path.c_str());
    }
}
