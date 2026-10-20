#include "host_directory_provider.hpp"

#include <string_view>

#include <platform/compiler.hpp>
#include <platform/unicode.hpp>

#ifdef OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

#ifdef OS_WINDOWS
        struct find_handle_deleter
        {
            void operator()(void* handle) const noexcept
            {
                (void)FindClose(handle);
            }
        };
#endif
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

#ifdef OS_WINDOWS
        std::wstring search_path = host_path->wstring();
        if (!search_path.ends_with(L'\\') && !search_path.ends_with(L'/'))
        {
            search_path.push_back(L'\\');
        }
        search_path.push_back(L'*');

        WIN32_FIND_DATAW find_data{};
        HANDLE find_handle = FindFirstFileW(search_path.c_str(), &find_data);
        if (find_handle == INVALID_HANDLE_VALUE)
        {
            return entries;
        }
        const std::unique_ptr<void, find_handle_deleter> find_handle_guard{find_handle};

        do
        {
            const std::wstring_view filename{find_data.cFileName};
            if (filename == L"." || filename == L"..")
            {
                continue;
            }

            const std::filesystem::path file_path = *host_path / find_data.cFileName;
            directory_entry entry{
                .name = std::filesystem::path{find_data.cFileName}.u16string(),
                .short_name = std::filesystem::path{find_data.cAlternateFileName}.u16string(),
                .is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            };

            struct compat_stat file_stat{};
            if (compat_stat(file_path, &file_stat))
            {
                entry.stat = file_stat;
            }

            entries.emplace_back(std::move(entry));
        } while (FindNextFileW(find_handle_guard.get(), &find_data));
#else
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
#endif

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
