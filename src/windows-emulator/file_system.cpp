#include "std_include.hpp"
#include "file_system.hpp"
#include "filesystem/host_directory_provider.hpp"

#include <sys/stat.h>

namespace sogen
{

    namespace
    {
        bool is_directory(const struct compat_stat& file_stat)
        {
            return (file_stat.st_mode & S_IFMT) == S_IFDIR;
        }

        bool is_protected_mount_root(const mount_provider& provider, const windows_path& relative)
        {
            if (!relative.empty())
            {
                return false;
            }

            const auto root_stat = provider.stat({});
            return !root_stat || is_directory(*root_stat);
        }

        NTSTATUS copy_file(i_file& source, i_file& destination)
        {
            std::vector<std::byte> buffer(0x10000);

            while (true)
            {
                const auto bytes_read = source.read(buffer);
                if (bytes_read == 0)
                {
                    break;
                }

                if (destination.write(std::span(buffer).first(bytes_read)) != bytes_read)
                {
                    return STATUS_ACCESS_DENIED;
                }
            }

            return destination.flush() ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
        }
    }

    file_system::file_system(const std::filesystem::path& root, const filesystem_mode root_mode)
    {
        if (root.empty())
        {
#ifdef OS_WINDOWS
            this->host_passthrough_ = true;

            const auto drive_bits = GetLogicalDrives();
            for (char drive = 'a'; drive <= 'z'; ++drive)
            {
                if (drive_bits & (1 << (drive - 'a')))
                {
                    const std::array<char, 4> drive_root{drive, ':', '\\', '\0'};
                    this->mount(windows_path{drive, {}}, std::make_unique<host_directory_provider>(drive_root.data()), root_mode);
                }
            }
#endif
            return;
        }

        const auto canonical_root = canonical(root);

        std::error_code ec{};
        for (const auto& entry : std::filesystem::directory_iterator(canonical_root, ec))
        {
            const auto filename = entry.path().filename().string();
            if (filename.size() == 1 && entry.is_directory(ec))
            {
                this->mount(windows_path{filename.front(), {}}, std::make_unique<host_directory_provider>(entry.path()), root_mode);
            }
        }
    }

    void file_system::mount(const windows_path& guest_path, std::unique_ptr<mount_provider> provider, const filesystem_mode mode)
    {
        if (!guest_path.is_absolute())
        {
            throw std::runtime_error("Only absolute paths can be mounted: " + guest_path.string());
        }

        this->mounts_[guest_path.lexically_normal()] = mount_entry{
            .provider = std::move(provider),
            .mode = mode,
        };
    }

    void file_system::map(const windows_path& guest_path, std::filesystem::path host_path, const filesystem_mode mode)
    {
        if (mode == filesystem_mode::copy_on_write)
        {
            throw std::runtime_error("A copy-on-write mount needs a layered provider: " + guest_path.string());
        }

        this->mount(guest_path, std::make_unique<host_directory_provider>(std::move(host_path)), mode);
    }

    std::set<char> file_system::list_drives() const
    {
        std::set<char> drives{};

        for (const auto& mount_point : this->mounts_ | std::views::keys)
        {
            drives.insert(*mount_point.get_drive());
        }

        return drives;
    }

    uint8_t file_system::drive_type(const char drive) const
    {
        const auto drives = this->list_drives();
        if (!drives.contains(utils::string::char_to_lower(drive)))
        {
            constexpr uint8_t no_root_directory = 1;
            return no_root_directory;
        }

#ifdef OS_WINDOWS
        if (this->host_passthrough_)
        {
            const std::array<wchar_t, 4> path{static_cast<wchar_t>(drive), L':', L'\\', L'\0'};
            return static_cast<uint8_t>(GetDriveTypeW(path.data()));
        }
#endif

        constexpr uint8_t fixed_drive_type = 3;
        return fixed_drive_type;
    }

    std::optional<file_system::resolved_path> file_system::resolve(const windows_path& path) const
    {
        const auto normalized = path.lexically_normal();

        std::optional<resolved_path> best{};

        for (const auto& [mount_point, entry] : this->mounts_)
        {
            auto relative = normalized.relative_to(mount_point);
            if (relative && (!best || mount_point.depth() > best->mount_point.depth()))
            {
                best = resolved_path{
                    .provider = entry.provider.get(),
                    .mode = entry.mode,
                    .mount_point = mount_point,
                    .relative = std::move(*relative),
                };
            }
        }

        return best;
    }

    std::optional<struct compat_stat> file_system::stat(const windows_path& path) const
    {
        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return std::nullopt;
        }

        return resolved->provider->stat(resolved->relative);
    }

    std::vector<directory_entry> file_system::list_directory(const windows_path& path) const
    {
        std::vector<directory_entry> entries{};

        if (const auto resolved = this->resolve(path))
        {
            entries = resolved->provider->list(resolved->relative);
        }

        const auto normalized = path.lexically_normal();

        for (const auto& [mount_point, entry] : this->mounts_)
        {
            if (mount_point.depth() == 0 || mount_point.parent() != normalized)
            {
                continue;
            }

            const auto mount_stat = entry.provider->stat({});
            if (!mount_stat)
            {
                continue;
            }

            auto name = mount_point.leaf();

            std::erase_if(entries, [&](const directory_entry& existing) {
                return utils::string::to_lower(existing.name) == name; //
            });

            entries.emplace_back(directory_entry{
                .name = std::move(name),
                .is_directory = is_directory(*mount_stat),
                .stat = mount_stat,
            });
        }

        return entries;
    }

    bool file_system::is_writable(const windows_path& path) const
    {
        const auto resolved = this->resolve(path);
        return resolved && allows_writes(resolved->mode);
    }

    open_result file_system::open(const windows_path& path, const std::u16string& mode)
    {
        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return {.status = STATUS_OBJECT_PATH_NOT_FOUND};
        }

        if (is_write_open_mode(mode) && !allows_writes(resolved->mode))
        {
            return {.status = STATUS_ACCESS_DENIED};
        }

        return resolved->provider->open(resolved->relative, mode);
    }

    open_result file_system::open_read(const windows_path& path) const
    {
        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return {.status = STATUS_OBJECT_PATH_NOT_FOUND};
        }

        return resolved->provider->open(resolved->relative, u"rb");
    }

    NTSTATUS file_system::create_directory(const windows_path& path)
    {
        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        if (!allows_writes(resolved->mode))
        {
            return STATUS_ACCESS_DENIED;
        }

        return resolved->provider->create_directory(resolved->relative);
    }

    NTSTATUS file_system::rename(const windows_path& from, const windows_path& to)
    {
        const auto source = this->resolve(from);
        const auto destination = this->resolve(to);
        if (!source || !destination)
        {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        if (is_protected_mount_root(*source->provider, source->relative) ||
            is_protected_mount_root(*destination->provider, destination->relative) || !allows_writes(source->mode) ||
            !allows_writes(destination->mode))
        {
            return STATUS_ACCESS_DENIED;
        }

        if (source->mount_point.get_drive() != destination->mount_point.get_drive())
        {
            return STATUS_NOT_SAME_DEVICE;
        }

        if (source->provider == destination->provider)
        {
            return source->provider->rename(source->relative, destination->relative);
        }

        const auto source_stat = source->provider->stat(source->relative);
        if (!source_stat)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (is_directory(*source_stat))
        {
            return STATUS_NOT_SAME_DEVICE;
        }

        {
            const auto input = source->provider->open(source->relative, u"rb");
            if (!input.file)
            {
                return input.status;
            }

            const auto output = destination->provider->open(destination->relative, u"wb");
            if (!output.file)
            {
                return output.status;
            }

            const auto status = copy_file(*input.file, *output.file);
            if (status != STATUS_SUCCESS)
            {
                return status;
            }
        }

        return source->provider->remove(source->relative);
    }

    NTSTATUS file_system::remove(const windows_path& path)
    {
        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        if (is_protected_mount_root(*resolved->provider, resolved->relative) || !allows_writes(resolved->mode))
        {
            return STATUS_ACCESS_DENIED;
        }

        return resolved->provider->remove(resolved->relative);
    }

    bool file_system::read_file(const windows_path& path, std::vector<std::byte>* data) const
    {
        const auto input = this->open_read(path);
        if (!input.file)
        {
            return false;
        }

        const auto size = input.file->size();
        if (size < 0 || !input.file->seek(0, SEEK_SET))
        {
            return false;
        }

        data->resize(static_cast<size_t>(size));

        size_t offset = 0;
        while (offset < data->size())
        {
            const auto read = input.file->read(std::span{*data}.subspan(offset));
            if (read == 0)
            {
                return false;
            }

            offset += read;
        }

        return true;
    }

    std::vector<std::byte> file_system::read_file(const windows_path& path) const
    {
        std::vector<std::byte> data{};
        if (!this->read_file(path, &data))
        {
            return {};
        }

        return data;
    }

    std::filesystem::path file_system::translate(const windows_path& path) const
    {
        if (!path.is_absolute())
        {
            throw std::runtime_error("Only absolute paths can be translated: " + path.string());
        }

        const auto resolved = this->resolve(path);
        if (!resolved)
        {
            return {};
        }

        return resolved->provider->get_host_path(resolved->relative).value_or(std::filesystem::path{});
    }

} // namespace sogen
