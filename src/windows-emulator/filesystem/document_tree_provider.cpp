#include "document_tree_provider.hpp"

#include <sys/stat.h>

#include <platform/unicode.hpp>

namespace sogen
{

    namespace
    {
        std::list<std::u16string> split_components(const std::u16string_view path)
        {
            std::list<std::u16string> components{};
            size_t start = 0;
            while (start <= path.size())
            {
                const auto end = std::min(path.find(u'/', start), path.size());
                components.emplace_back(path.substr(start, end - start));
                start = end + 1;
            }

            return components;
        }

        windows_path to_relative_path(const document_entry& entry)
        {
            auto components = split_components(entry.path);
            for (const auto& component : components)
            {
                if (component.empty() || component == u"." || component == u".." || component.find(u'\\') != std::u16string::npos)
                {
                    throw std::runtime_error("Invalid document path: " + u16_to_u8(entry.path));
                }
            }

            return windows_path{std::nullopt, std::move(components)};
        }

        // Document IDs survive renames inside the provider, unlike paths, so they make the stable file ID.
        uint64_t file_id(const std::string_view document_id)
        {
            uint64_t hash = 0xcbf29ce484222325;
            for (const auto c : document_id)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= 0x100000001b3;
            }

            return hash;
        }

        timespec to_timespec(const int64_t milliseconds)
        {
            timespec time{};
            time.tv_sec = static_cast<decltype(time.tv_sec)>(milliseconds / 1000);
            time.tv_nsec = static_cast<decltype(time.tv_nsec)>((milliseconds % 1000) * 1000000);
            return time;
        }

        struct compat_stat make_stat(const document_entry& entry)
        {
            struct compat_stat result{};
            result.st_mode = entry.is_directory ? (S_IFDIR | 0555) : (S_IFREG | 0444);
            result.st_nlink = 1;
            result.st_ino = file_id(entry.id);
            result.st_size = entry.is_directory ? 0 : static_cast<int64_t>(entry.size);
            result.st_atimespec = to_timespec(entry.modified_ms);
            result.st_mtimespec = result.st_atimespec;
            result.st_ctimespec = result.st_atimespec;
            return result;
        }

        // Reports the identity of the listing for the open file too, so a handle and a directory query agree on it.
        class document_file final : public i_file
        {
          public:
            document_file(std::unique_ptr<i_file> file, const struct compat_stat& identity)
                : file_(std::move(file)),
                  identity_(identity)
            {
            }

            size_t read(const std::span<std::byte> buffer) override
            {
                return this->file_->read(buffer);
            }

            size_t write(const std::span<const std::byte> buffer) override
            {
                return this->file_->write(buffer);
            }

            bool flush() override
            {
                return this->file_->flush();
            }

            bool seek(const int64_t offset, const int origin) override
            {
                return this->file_->seek(offset, origin);
            }

            int64_t tell() override
            {
                return this->file_->tell();
            }

            int64_t size() override
            {
                return this->file_->size();
            }

            bool resize(const uint64_t size) override
            {
                return this->file_->resize(size);
            }

            std::optional<struct compat_stat> stat() override
            {
                auto result = this->identity_;
                result.st_size = this->file_->size();
                return result;
            }

          private:
            std::unique_ptr<i_file> file_{};
            struct compat_stat identity_{};
        };

        struct compat_stat make_root_stat()
        {
            struct compat_stat result{};
            result.st_mode = S_IFDIR | 0555;
            result.st_nlink = 1;
            return result;
        }
    }

    void document_entry::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_string(this->path);
        buffer.write_string(this->id);
        buffer.write(this->is_directory);
        buffer.write(this->size);
        buffer.write(this->modified_ms);
    }

    void document_entry::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_string(this->path);
        buffer.read_string(this->id);
        buffer.read(this->is_directory);
        buffer.read(this->size);
        buffer.read(this->modified_ms);
    }

    document_tree_provider::document_tree_provider(std::vector<document_entry> entries, std::shared_ptr<document_source> source)
        : entries_(std::move(entries)),
          source_(std::move(source))
    {
        for (size_t i = 0; i < this->entries_.size(); ++i)
        {
            const auto path = to_relative_path(this->entries_[i]);
            const auto [existing, inserted] = this->by_path_.emplace(path, i);
            if (!inserted)
            {
                throw std::runtime_error("Document paths differ only in case: " + u16_to_u8(this->entries_[existing->second].path) +
                                         " and " + u16_to_u8(this->entries_[i].path));
            }

            this->children_[path.parent()].push_back(i);
        }

        for (const auto& parent : this->children_ | std::views::keys)
        {
            if (parent.empty())
            {
                continue;
            }

            const auto* entry = this->find(parent);
            if (!entry || !entry->is_directory)
            {
                throw std::runtime_error("Document tree lists entries below a missing directory: " + parent.string());
            }
        }
    }

    const document_entry* document_tree_provider::find(const windows_path& path) const
    {
        const auto entry = this->by_path_.find(path);
        return entry == this->by_path_.end() ? nullptr : &this->entries_[entry->second];
    }

    std::optional<struct compat_stat> document_tree_provider::stat(const windows_path& path) const
    {
        if (path.empty())
        {
            return make_root_stat();
        }

        const auto* entry = this->find(path);
        if (!entry)
        {
            return std::nullopt;
        }

        return make_stat(*entry);
    }

    std::vector<directory_entry> document_tree_provider::list(const windows_path& path) const
    {
        std::vector<directory_entry> entries{};

        const auto children = this->children_.find(path);
        if (children == this->children_.end())
        {
            return entries;
        }

        entries.reserve(children->second.size());
        for (const auto index : children->second)
        {
            const auto& entry = this->entries_[index];
            entries.push_back(directory_entry{
                .name = split_components(entry.path).back(),
                .is_directory = entry.is_directory,
                .stat = make_stat(entry),
            });
        }

        return entries;
    }

    open_result document_tree_provider::open(const windows_path& path, const std::u16string& mode)
    {
        if (is_write_open_mode(mode))
        {
            return {.status = STATUS_ACCESS_DENIED};
        }

        const auto* entry = this->find(path);
        if (!entry)
        {
            return {.status = path.empty() ? STATUS_FILE_IS_A_DIRECTORY : STATUS_OBJECT_NAME_NOT_FOUND};
        }

        if (entry->is_directory)
        {
            return {.status = STATUS_FILE_IS_A_DIRECTORY};
        }

        auto file = this->source_->open(entry->id);
        if (!file)
        {
            return {.status = STATUS_ACCESS_DENIED};
        }

        return {.file = std::make_unique<document_file>(std::move(file), make_stat(*entry)), .status = STATUS_SUCCESS};
    }

    NTSTATUS document_tree_provider::create_directory(const windows_path& /*path*/)
    {
        return STATUS_ACCESS_DENIED;
    }

    NTSTATUS document_tree_provider::rename(const windows_path& /*from*/, const windows_path& /*to*/)
    {
        return STATUS_ACCESS_DENIED;
    }

    NTSTATUS document_tree_provider::remove(const windows_path& /*path*/)
    {
        return STATUS_ACCESS_DENIED;
    }

    std::optional<std::filesystem::path> document_tree_provider::get_host_path(const windows_path& /*path*/) const
    {
        return std::nullopt;
    }

} // namespace sogen
