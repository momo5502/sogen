#include "host_directory_provider.hpp"

#include <platform/compiler.hpp>
#include <platform/unicode.hpp>

#if defined(OS_WINDOWS) && !defined(__MINGW64__)
#include <corecrt_io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{

    namespace
    {
        bool is_subpath(const std::filesystem::path& root, const std::filesystem::path& target)
        {
            const auto relative_path = target.lexically_relative(root);
            return !relative_path.empty() && *relative_path.begin() != "..";
        }

        NTSTATUS map_open_error(const int error)
        {
            switch (error)
            {
            case ENOENT:
                return STATUS_OBJECT_NAME_NOT_FOUND;
            case EACCES:
                return STATUS_ACCESS_DENIED;
            case EISDIR:
                return STATUS_FILE_IS_A_DIRECTORY;
            default:
                return STATUS_NOT_SUPPORTED;
            }
        }
    }

    host_file::host_file(FILE* file)
        : file_(file)
    {
    }

    host_file::~host_file()
    {
        if (this->file_)
        {
            (void)fclose(this->file_);
        }
    }

    size_t host_file::read(const std::span<std::byte> buffer)
    {
        return fread(buffer.data(), 1, buffer.size(), this->file_);
    }

    size_t host_file::write(const std::span<const std::byte> buffer)
    {
        return fwrite(buffer.data(), 1, buffer.size(), this->file_);
    }

    bool host_file::flush()
    {
        return fflush(this->file_) == 0;
    }

    bool host_file::seek(const int64_t offset, const int origin)
    {
        return _fseeki64(this->file_, offset, origin) == 0;
    }

    int64_t host_file::tell()
    {
        return _ftelli64(this->file_);
    }

    int64_t host_file::size()
    {
        const auto current_position = this->tell();

        this->seek(0, SEEK_END);
        const auto size = this->tell();
        this->seek(current_position, SEEK_SET);

        return size;
    }

    bool host_file::resize(const uint64_t size)
    {
        const auto fd = fileno(this->file_);
        if (fd == -1)
        {
            return false;
        }

#ifdef OS_WINDOWS
        // NOLINTNEXTLINE(google-runtime-int)
        return _chsize_s(fd, static_cast<long long>(size)) == 0;
#else
        return ftruncate(fd, static_cast<off_t>(size)) == 0;
#endif
    }

    std::optional<struct compat_stat> host_file::stat()
    {
        struct compat_stat file_stat{};
        if (!compat_fstat(fileno(this->file_), &file_stat))
        {
            return std::nullopt;
        }

        return file_stat;
    }

    host_directory_provider::host_directory_provider(std::filesystem::path target)
        : target_(std::move(target))
    {
    }

    std::optional<std::filesystem::path> host_directory_provider::get_host_path(const windows_path& path) const
    {
        if (path.empty())
        {
            return this->target_;
        }

        auto candidate = (this->target_ / path.to_portable_path()).lexically_normal();
        // The containment check is lexical and does not follow symlinks. Host symlinks inside the mount may
        // point elsewhere (e.g. a mounted game directory); the OS resolves those at open time.
        if (!is_subpath(this->target_.lexically_normal(), candidate))
        {
            return std::nullopt;
        }

        std::error_code ec{};
        auto resolved = weakly_canonical(candidate, ec);
        if (ec)
        {
            return candidate;
        }

        return resolved;
    }

    std::optional<struct compat_stat> host_directory_provider::stat(const windows_path& path) const
    {
        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return std::nullopt;
        }

        struct compat_stat file_stat{};
        if (!compat_stat(*host_path, &file_stat))
        {
            return std::nullopt;
        }

        return file_stat;
    }

    std::vector<directory_entry> host_directory_provider::list(const windows_path& path) const
    {
        std::vector<directory_entry> entries{};

        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return entries;
        }

        std::error_code ec{};
        for (const auto& file : std::filesystem::directory_iterator(*host_path, ec))
        {
            directory_entry entry{
                .name = file.path().filename().u16string(),
                .is_directory = file.is_directory(ec),
            };

            struct compat_stat file_stat{};
            if (compat_stat(file.path(), &file_stat))
            {
                entry.stat = file_stat;
            }

            entries.emplace_back(std::move(entry));
        }

        return entries;
    }

    open_result host_directory_provider::open(const windows_path& path, const std::u16string& mode)
    {
        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return {.status = STATUS_ACCESS_DENIED};
        }

        FILE* file{};
        const auto error = open_unicode(&file, *host_path, mode);
        if (!file)
        {
            return {.status = map_open_error(error)};
        }

        return {.file = std::make_unique<host_file>(file), .status = STATUS_SUCCESS};
    }

    NTSTATUS host_directory_provider::create_directory(const windows_path& path)
    {
        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return STATUS_ACCESS_DENIED;
        }

        std::error_code ec{};
        if (std::filesystem::exists(*host_path, ec))
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (!std::filesystem::is_directory(host_path->parent_path(), ec))
        {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        std::filesystem::create_directory(*host_path, ec);
        return ec ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    }

    NTSTATUS host_directory_provider::rename(const windows_path& from, const windows_path& to)
    {
        const auto host_from = this->get_host_path(from);
        const auto host_to = this->get_host_path(to);
        if (!host_from || !host_to)
        {
            return STATUS_ACCESS_DENIED;
        }

        std::error_code ec{};
        std::filesystem::rename(*host_from, *host_to, ec);
        return ec ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
    }

    NTSTATUS host_directory_provider::remove(const windows_path& path)
    {
        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return STATUS_ACCESS_DENIED;
        }

        std::error_code ec{};
        const auto removed = std::filesystem::remove(*host_path, ec);
        if (ec)
        {
            return STATUS_ACCESS_DENIED;
        }

        return removed ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
    }

} // namespace sogen
