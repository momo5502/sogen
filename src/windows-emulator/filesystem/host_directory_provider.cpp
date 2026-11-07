#include "host_directory_provider.hpp"

#include <cwctype>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <platform/compiler.hpp>
#include <platform/unicode.hpp>
#include <utils/string.hpp>

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

#ifdef OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

namespace sogen
{

    namespace
    {
        // Windows junctions are reported as an implementation-defined file type rather than as symlinks.
        bool is_link(const std::filesystem::file_status status)
        {
            return std::filesystem::is_symlink(status) || std::filesystem::is_other(status);
        }

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
        // Windows only re-creates an existing hidden or system file when the new file keeps those attributes, which is
        // how an NT caller overwrites it. The guest does not see host attributes, so it cannot ask for them itself.
        FILE* recreate_keeping_attributes(const std::filesystem::path& path, const std::u16string& mode)
        {
            constexpr DWORD kept_attributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & kept_attributes) == 0)
            {
                return nullptr;
            }

            const bool readable = mode.find(u'+') != std::u16string::npos;
            auto* const handle = CreateFileW(path.c_str(), GENERIC_WRITE | (readable ? GENERIC_READ : 0),
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                                             attributes & kept_attributes, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return nullptr;
            }

            const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | (readable ? _O_RDWR : _O_WRONLY));
            if (descriptor == -1)
            {
                CloseHandle(handle);
                return nullptr;
            }

            auto* file = _wfdopen(descriptor, u16_to_w(mode).c_str());
            if (!file)
            {
                _close(descriptor);
            }

            return file;
        }
#endif

        std::optional<std::filesystem::path> find_entry_ignoring_case(const std::filesystem::path& directory,
                                                                      const std::filesystem::path& name)
        {
            const auto wanted = name.u16string();
            std::optional<std::u16string> match{};

            std::error_code ec{};
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
            {
                auto candidate = entry.path().filename().u16string();
                if (!utils::string::equals_ignore_case(candidate, wanted) || (match && *match < candidate))
                {
                    continue;
                }

                match = std::move(candidate);
            }

            if (!match)
            {
                return std::nullopt;
            }

            return std::filesystem::path{*match};
        }

        // With refused links, the status of an entry is taken without following it, so that the one lookup also
        // tells whether the entry is a link.
        // A failed lookup reports file_type::none, which is kept apart from an entry that does not exist.
        std::filesystem::file_status probe_host_entry(const std::filesystem::path& path, const host_links links)
        {
            std::error_code ec{};
            return links == host_links::follow ? std::filesystem::status(path, ec) : std::filesystem::symlink_status(path, ec);
        }

        bool lookup_failed(const std::filesystem::file_status status)
        {
            return status.type() == std::filesystem::file_type::none;
        }

        // With refused links, each component is checked before anything is looked up beneath it, and a path through a
        // link comes back empty.
        std::optional<std::filesystem::path> recover_host_case(const std::filesystem::path& root, const std::filesystem::path& candidate,
                                                               const host_links links)
        {
            const auto relative = candidate.lexically_relative(root);
            if (relative.empty())
            {
                return candidate;
            }

            auto resolved = root;
            auto component = relative.begin();

            for (; component != relative.end(); ++component)
            {
                if (component->empty())
                {
                    continue;
                }

                // The emulation root is created in lower case, so that is the spelling almost every mismatched
                // component has on a case sensitive host. Trying it first keeps a failed probe deep in System32
                // from scanning every directory on the way down.
                const std::filesystem::path lowered{utils::string::to_lower(component->u16string())};
                std::optional<std::filesystem::path> match{};
                auto status = probe_host_entry(resolved / *component, links);
                auto failed = lookup_failed(status);
                if (std::filesystem::exists(status))
                {
                    match = *component;
                }
                else if (status = probe_host_entry(resolved / lowered, links); std::filesystem::exists(status))
                {
                    match = lowered;
                }
                else
                {
                    failed = failed || lookup_failed(status);
                    match = find_entry_ignoring_case(resolved, *component);
                    if (match && links == host_links::refuse)
                    {
                        status = probe_host_entry(resolved / *match, links);
                    }
                }

                if (links == host_links::refuse && (failed || (match && (!std::filesystem::exists(status) || is_link(status)))))
                {
                    return std::nullopt;
                }

                if (!match)
                {
                    break;
                }

                resolved /= *match;
            }

            // Everything from the first component that has no counterpart on the host is kept as asked, because
            // create_directory and the destination side of rename translate a path that must not exist yet and
            // then validate its parent.
            for (; component != relative.end(); ++component)
            {
                if (!component->empty())
                {
                    resolved /= *component;
                }
            }

            return resolved;
        }

        bool entry_lookup_ignores_case(const std::filesystem::path& entry)
        {
            const auto name = entry.filename().u16string();

            std::u16string flipped{};
            flipped.reserve(name.size());
            bool flipped_any = false;

            for (const auto c : name)
            {
                const auto lower = utils::string::char_to_lower(c);
                const auto upper = static_cast<char16_t>(std::towupper(static_cast<std::wint_t>(c)));

                if (lower != c)
                {
                    flipped.push_back(lower);
                    flipped_any = true;
                }
                else if (upper != c)
                {
                    flipped.push_back(upper);
                    flipped_any = true;
                }
                else
                {
                    flipped.push_back(c);
                }
            }

            if (!flipped_any)
            {
                return false;
            }

            std::error_code ec{};
            return std::filesystem::equivalent(entry, entry.parent_path() / std::filesystem::path(flipped), ec);
        }

        bool host_lookup_ignores_case(const std::filesystem::path& target)
        {
            std::error_code ec{};
            if (!std::filesystem::is_directory(target, ec))
            {
                return entry_lookup_ignores_case(target);
            }

            for (const auto& entry : std::filesystem::directory_iterator(target, ec))
            {
                if (!entry.is_symlink(ec) && entry_lookup_ignores_case(entry.path()))
                {
                    return true;
                }
            }

            return false;
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

    host_directory_provider::host_directory_provider(std::filesystem::path target, const host_links links)
        : target_(std::move(target)),
          links_(links),
          lookup_ignores_case_(host_lookup_ignores_case(this->target_))
    {
    }

    bool host_directory_provider::avoids_links(const std::filesystem::path& root, const std::filesystem::path& host_path)
    {
        auto current = root;
        for (const auto& component : host_path.lexically_relative(root))
        {
            current /= component;
            std::error_code ec{};
            const auto status = std::filesystem::symlink_status(current, ec);
            if (status.type() == std::filesystem::file_type::not_found)
            {
                return true;
            }
            if (!std::filesystem::exists(status) || is_link(status))
            {
                return false;
            }
        }

        return true;
    }

    std::optional<std::filesystem::path> host_directory_provider::get_host_path(const windows_path& path) const
    {
        if (path.empty())
        {
            return this->target_;
        }

        auto candidate = (this->target_ / path.to_portable_path()).lexically_normal();
        const auto root = this->target_.lexically_normal();
        // The containment check is lexical and does not follow symlinks. Unless they are refused, host symlinks
        // inside the mount may point elsewhere (e.g. a mounted game directory); the OS resolves those at open time.
        if (!is_subpath(root, candidate))
        {
            return std::nullopt;
        }

        // A normal path that passes through no link below the root needs no canonicalization.
        if (this->links_ == host_links::refuse)
        {
            if (!this->lookup_ignores_case_)
            {
                return recover_host_case(root, candidate, this->links_);
            }

            return avoids_links(root, candidate) ? std::optional{candidate} : std::nullopt;
        }

        std::error_code ec{};
        if (!this->lookup_ignores_case_)
        {
            // The emulation root is created in lower case, so that spelling is the hit for almost every guest path
            // and is tried first, in one stat. The guest's own spelling comes next, for entries the guest created,
            // and only a path that is neither is walked component by component.
            auto lowered =
                (this->target_ / std::filesystem::path{utils::string::to_lower(path.to_portable_path().u16string())}).lexically_normal();

            if (is_subpath(root, lowered) && std::filesystem::exists(lowered, ec))
            {
                candidate = std::move(lowered);
            }
            else if (!std::filesystem::exists(candidate, ec))
            {
                candidate = *recover_host_case(root, candidate, this->links_);
            }
        }

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
        std::unordered_map<std::u16string, std::u16string> short_names{};

        std::wstring search_path = host_path->wstring();
        if (!search_path.ends_with(L'\\') && !search_path.ends_with(L'/'))
        {
            search_path.push_back(L'\\');
        }
        search_path.push_back(L'*');

        WIN32_FIND_DATAW find_data{};
        HANDLE find_handle = FindFirstFileW(search_path.c_str(), &find_data);
        if (find_handle != INVALID_HANDLE_VALUE)
        {
            const std::unique_ptr<void, find_handle_deleter> find_handle_guard{find_handle};

            do
            {
                const std::wstring_view filename{find_data.cFileName};
                const std::wstring_view short_name{find_data.cAlternateFileName};
                if (filename == L"." || filename == L".." || short_name.empty())
                {
                    continue;
                }

                short_names.emplace(std::filesystem::path{find_data.cFileName}.u16string(),
                                    std::filesystem::path{find_data.cAlternateFileName}.u16string());
            } while (FindNextFileW(find_handle_guard.get(), &find_data));
        }
#endif

        std::error_code ec{};
        for (const auto& file : std::filesystem::directory_iterator(*host_path, ec))
        {
            bool is_directory{};
            if (this->links_ == host_links::refuse)
            {
                std::error_code status_error{};
                const auto status = file.symlink_status(status_error);
                if (status_error || !std::filesystem::exists(status) || is_link(status))
                {
                    continue;
                }

                is_directory = std::filesystem::is_directory(status);
            }
            else
            {
                is_directory = file.is_directory(ec);
            }

            directory_entry entry{
                .name = file.path().filename().u16string(),
                .is_directory = is_directory,
            };

#ifdef OS_WINDOWS
            const auto short_name = short_names.find(entry.name);
            if (short_name != short_names.end())
            {
                entry.short_name = short_name->second;
            }
#endif

            struct compat_stat file_stat{};
            if (compat_stat(file.path(), &file_stat))
            {
                entry.stat = file_stat;
            }

            entries.emplace_back(std::move(entry));
        }

        return entries;
    }

    std::vector<std::u16string> host_directory_provider::list_names(const windows_path& path) const
    {
        std::vector<std::u16string> names{};
        const auto host_path = this->get_host_path(path);
        if (!host_path)
        {
            return names;
        }

        std::error_code ec{};
        for (const auto& file : std::filesystem::directory_iterator(*host_path, ec))
        {
            if (this->links_ == host_links::refuse)
            {
                std::error_code status_error{};
                const auto status = file.symlink_status(status_error);
                if (status_error || !std::filesystem::exists(status) || is_link(status))
                {
                    continue;
                }
            }

            names.push_back(file.path().filename().u16string());
        }
        return names;
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
#ifdef OS_WINDOWS
        if (!file && error == EACCES && mode.starts_with(u'w'))
        {
            file = recreate_keeping_attributes(*host_path, mode);
        }
#endif
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
