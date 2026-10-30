#include "layered_provider.hpp"
#include "host_directory_provider.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <platform/compiler.hpp>
#include <platform/unicode.hpp>
#include <utils/finally.hpp>
#include <utils/string.hpp>

#ifdef OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

namespace sogen
{

    namespace
    {
        constexpr char16_t write_mode = u'w';

        bool is_directory(const struct compat_stat& file_stat)
        {
            return (file_stat.st_mode & S_IFMT) == S_IFDIR;
        }

        bool sync_stream(FILE* stream)
        {
            if (fflush(stream) != 0)
            {
                return false;
            }

#ifdef OS_WINDOWS
            return _commit(_fileno(stream)) == 0;
#else
            return fsync(fileno(stream)) == 0;
#endif
        }

        struct stream_closer
        {
            void operator()(FILE* stream) const noexcept
            {
                (void)fclose(stream);
            }
        };

        using unique_stream = std::unique_ptr<FILE, stream_closer>;

        std::vector<windows_path> ancestors_of(const windows_path& path)
        {
            std::vector<windows_path> result{};
            for (auto current = path; current.depth() > 0; current = current.parent())
            {
                result.push_back(current);
            }

            std::ranges::reverse(result);
            return result;
        }
    }

    layer_journal::layer_journal(std::filesystem::path file)
        : file_(std::move(file))
    {
        std::ifstream stream(this->file_, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        stream.close();

        // A record the crash cut short has no terminator. It is dropped and cut off, so that the next record does not
        // continue it.
        const auto last_newline = content.find_last_of('\n');
        const auto complete = last_newline == std::string::npos ? 0 : last_newline + 1;
        if (complete != content.size())
        {
            std::error_code ec{};
            std::filesystem::resize_file(this->file_, complete, ec);
            if (ec)
            {
                throw std::runtime_error("Failed to recover the layer journal: " + this->file_.string());
            }
        }

        std::istringstream records(content.substr(0, complete));
        std::string line{};
        while (std::getline(records, line))
        {
            if (line.size() < 3 || line[1] != '\t')
            {
                continue;
            }

            const windows_path path{std::filesystem::path{u8_to_u16(std::string_view{line}.substr(2))}};
            if (line[0] == 'H')
            {
                this->hidden_.insert(path);
            }
            else if (line[0] == 'O')
            {
                this->opaque_.insert(path);
            }
        }
    }

    bool layer_journal::is_hidden(const windows_path& path) const
    {
        return this->hidden_.contains(path);
    }

    bool layer_journal::hides_lower_children(const windows_path& path) const
    {
        return this->opaque_.contains(path);
    }

    bool layer_journal::hide(const windows_path& path)
    {
        if (this->hidden_.contains(path))
        {
            return true;
        }

        if (!this->append('H', path))
        {
            return false;
        }

        this->hidden_.insert(path);
        return true;
    }

    bool layer_journal::hide_lower_children(const windows_path& path)
    {
        if (this->opaque_.contains(path))
        {
            return true;
        }

        if (!this->append('O', path))
        {
            return false;
        }

        this->opaque_.insert(path);
        return true;
    }

    bool layer_journal::append(const char kind, const windows_path& path)
    {
        std::error_code ec{};
        std::filesystem::create_directories(this->file_.parent_path(), ec);

        FILE* raw{};
        open_unicode(&raw, this->file_, u"ab");
        if (!raw)
        {
            return false;
        }

        const unique_stream stream{raw};
        const auto record = std::string{kind} + '\t' + path.string() + '\n';
        return fwrite(record.data(), 1, record.size(), stream.get()) == record.size() && sync_stream(stream.get());
    }

    class cow_file final : public i_file
    {
      public:
        cow_file(layered_provider& provider, windows_path path, std::unique_ptr<i_file> lower, std::u16string mode)
            : provider_(&provider),
              path_(std::move(path)),
              lower_(std::move(lower)),
              mode_(std::move(mode))
        {
        }

        size_t read(const std::span<std::byte> buffer) override
        {
            return this->current().read(buffer);
        }

        size_t write(const std::span<const std::byte> buffer) override
        {
            return this->copy_up() ? this->current().write(buffer) : 0;
        }

        bool flush() override
        {
            return this->current().flush();
        }

        bool seek(const int64_t offset, const int origin) override
        {
            return this->current().seek(offset, origin);
        }

        int64_t tell() override
        {
            return this->current().tell();
        }

        int64_t size() override
        {
            return this->current().size();
        }

        bool resize(const uint64_t size) override
        {
            return this->copy_up() && this->current().resize(size);
        }

        std::optional<struct compat_stat> stat() override
        {
            return this->current().stat();
        }

      private:
        i_file& current() const
        {
            return this->upper_ ? *this->upper_ : *this->lower_;
        }

        bool copy_up()
        {
            if (this->upper_)
            {
                return true;
            }

            const auto position = this->lower_->tell();
            if (position < 0 || !this->provider_->copy_up(this->path_))
            {
                return false;
            }

            const bool appending = this->mode_.find(u'a') != std::u16string::npos;
            auto reopened = this->provider_->upper_->open(this->path_, appending ? this->mode_ : u"r+b");
            if (!reopened.file)
            {
                return false;
            }

            if (!appending && !reopened.file->seek(position, SEEK_SET))
            {
                return false;
            }

            this->upper_ = std::move(reopened.file);
            this->lower_.reset();
            return true;
        }

        layered_provider* provider_{};
        windows_path path_{};
        std::unique_ptr<i_file> lower_{};
        std::unique_ptr<i_file> upper_{};
        std::u16string mode_{};
    };

    layered_provider::layered_provider(std::unique_ptr<mount_provider> lower, std::filesystem::path upper_directory,
                                       const std::filesystem::path& state_directory)
        : lower_(std::move(lower)),
          temporary_directory_(state_directory / "tmp"),
          journal_(state_directory / "journal")
    {
        std::error_code ec{};
        std::filesystem::create_directories(upper_directory, ec);
        std::filesystem::create_directories(this->temporary_directory_, ec);

        // Copies a crash left half-written were never published.
        for (const auto& entry : std::filesystem::directory_iterator(this->temporary_directory_, ec))
        {
            std::filesystem::remove_all(entry.path(), ec);
        }

        this->upper_ = std::make_unique<host_directory_provider>(std::move(upper_directory));
    }

    bool layered_provider::lower_visible(const windows_path& path) const
    {
        if (this->journal_.is_hidden(path))
        {
            return false;
        }

        for (auto ancestor = path.parent(); ancestor.depth() > 0; ancestor = ancestor.parent())
        {
            if (this->journal_.is_hidden(ancestor) || this->journal_.hides_lower_children(ancestor))
            {
                return false;
            }
        }

        return true;
    }

    std::optional<struct compat_stat> layered_provider::lower_stat(const windows_path& path) const
    {
        if (!this->lower_visible(path))
        {
            return std::nullopt;
        }

        return this->lower_->stat(path);
    }

    std::optional<struct compat_stat> layered_provider::stat(const windows_path& path) const
    {
        if (auto upper = this->upper_->stat(path))
        {
            return upper;
        }

        return this->lower_stat(path);
    }

    std::vector<directory_entry> layered_provider::list(const windows_path& path) const
    {
        auto entries = this->upper_->list(path);

        std::unordered_set<std::u16string> names{};
        std::unordered_set<std::u16string> short_names{};
        for (const auto& entry : entries)
        {
            names.insert(utils::string::to_lower(entry.name));
            if (!entry.short_name.empty())
            {
                short_names.insert(utils::string::to_lower(entry.short_name));
            }
        }

        const auto lower_directory = this->lower_stat(path);
        if (!lower_directory || !is_directory(*lower_directory) || this->journal_.hides_lower_children(path))
        {
            return entries;
        }

        for (auto& entry : this->lower_->list(path))
        {
            const auto name = utils::string::to_lower(entry.name);
            if (names.contains(name) || this->journal_.is_hidden(path / windows_path{std::filesystem::path{name}}))
            {
                continue;
            }

            if (!entry.short_name.empty() && !short_names.insert(utils::string::to_lower(entry.short_name)).second)
            {
                entry.short_name.clear();
            }

            entries.push_back(std::move(entry));
        }

        return entries;
    }

    NTSTATUS layered_provider::create_upper_parents(const windows_path& path)
    {
        for (const auto& ancestor : ancestors_of(path.parent()))
        {
            if (const auto existing = this->upper_->stat(ancestor))
            {
                if (!is_directory(*existing))
                {
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                }

                continue;
            }

            const auto lower = this->lower_stat(ancestor);
            if (!lower || !is_directory(*lower))
            {
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }

            const auto status = this->upper_->create_directory(ancestor);
            if (status != STATUS_SUCCESS && status != STATUS_OBJECT_NAME_COLLISION)
            {
                return status;
            }
        }

        return STATUS_SUCCESS;
    }

    bool layered_provider::copy_to_upper(const windows_path& source, const windows_path& destination)
    {
        const auto input = this->lower_->open(source, u"rb");
        const auto target = this->upper_->get_host_path(destination);
        if (!input.file || !target)
        {
            return false;
        }

        std::error_code ec{};
        const auto temporary = this->temporary_directory_ / std::to_string(this->temporary_counter_++);
        auto cleanup = utils::finally([&] { std::filesystem::remove(temporary, ec); });

        {
            FILE* raw{};
            open_unicode(&raw, temporary, u"wb");
            if (!raw)
            {
                return false;
            }

            const unique_stream output{raw};
            std::vector<std::byte> buffer(0x10000);
            while (const auto count = input.file->read(buffer))
            {
                if (fwrite(buffer.data(), 1, count, output.get()) != count)
                {
                    return false;
                }
            }

            if (!sync_stream(output.get()))
            {
                return false;
            }
        }

        if (const auto source_stat = input.file->stat())
        {
            using std::chrono::duration_cast;
            using std::chrono::system_clock;

            const system_clock::time_point modified{duration_cast<system_clock::duration>(
                std::chrono::seconds{source_stat->st_mtimespec.tv_sec} + std::chrono::nanoseconds{source_stat->st_mtimespec.tv_nsec})};
            const auto file_now = std::filesystem::file_time_type::clock::now();
            const auto age = system_clock::now() - modified;
            std::filesystem::last_write_time(temporary, file_now - duration_cast<std::filesystem::file_time_type::duration>(age), ec);
        }

        std::filesystem::rename(temporary, *target, ec);
        return !ec;
    }

    bool layered_provider::copy_up(const windows_path& path)
    {
        if (this->upper_->stat(path))
        {
            return true;
        }

        return this->create_upper_parents(path) == STATUS_SUCCESS && this->copy_to_upper(path, path);
    }

    open_result layered_provider::open(const windows_path& path, const std::u16string& mode)
    {
        const auto upper = this->upper_->stat(path);

        if (!is_write_open_mode(mode))
        {
            if (upper)
            {
                return this->upper_->open(path, mode);
            }

            if (!this->lower_stat(path))
            {
                return {.status = STATUS_OBJECT_NAME_NOT_FOUND};
            }

            return this->lower_->open(path, mode);
        }

        if (upper)
        {
            return this->upper_->open(path, mode);
        }

        const auto lower = this->lower_stat(path);
        if (lower && is_directory(*lower))
        {
            return {.status = STATUS_FILE_IS_A_DIRECTORY};
        }

        // Opening for write truncates, so there is nothing to copy up.
        if (!lower || mode.find(write_mode) != std::u16string::npos)
        {
            if (const auto status = this->create_upper_parents(path); status != STATUS_SUCCESS)
            {
                return {.status = status};
            }

            return this->upper_->open(path, mode);
        }

        auto input = this->lower_->open(path, u"rb");
        if (!input.file)
        {
            return input;
        }

        return {.file = std::make_unique<cow_file>(*this, path, std::move(input.file), mode), .status = STATUS_SUCCESS};
    }

    NTSTATUS layered_provider::create_directory(const windows_path& path)
    {
        if (this->stat(path))
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (const auto status = this->create_upper_parents(path); status != STATUS_SUCCESS)
        {
            return status;
        }

        // A deleted lower directory must not bring its old content back when it is created again.
        const auto hidden = this->lower_->stat(path);
        if (hidden && is_directory(*hidden) && !this->journal_.hide_lower_children(path))
        {
            return STATUS_ACCESS_DENIED;
        }

        return this->upper_->create_directory(path);
    }

    NTSTATUS layered_provider::rename(const windows_path& from, const windows_path& to)
    {
        const auto source = this->stat(from);
        if (!source)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (const auto status = this->create_upper_parents(to); status != STATUS_SUCCESS)
        {
            return status;
        }

        const auto in_upper = this->upper_->stat(from).has_value();
        const auto in_lower = this->lower_stat(from).has_value();

        if (const auto destination = this->stat(to); destination && is_directory(*destination))
        {
            return STATUS_ACCESS_DENIED;
        }

        if (is_directory(*source))
        {
            if (in_lower)
            {
                return STATUS_NOT_SAME_DEVICE;
            }

            return this->upper_->rename(from, to);
        }

        if (in_upper)
        {
            // The old name is hidden first: the upper file keeps shadowing it until the rename is done.
            if (in_lower && !this->journal_.hide(from))
            {
                return STATUS_ACCESS_DENIED;
            }

            return this->upper_->rename(from, to);
        }

        // Replacing needs the old destination to survive a failed or interrupted rename, which has no durable record yet.
        if (this->stat(to))
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (!this->copy_to_upper(from, to))
        {
            return STATUS_ACCESS_DENIED;
        }

        if (!this->journal_.hide(from))
        {
            (void)this->upper_->remove(to);
            return STATUS_ACCESS_DENIED;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS layered_provider::remove(const windows_path& path)
    {
        const auto existing = this->stat(path);
        if (!existing)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        if (is_directory(*existing) && !this->list(path).empty())
        {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }

        if (this->lower_stat(path) && !this->journal_.hide(path))
        {
            return STATUS_ACCESS_DENIED;
        }

        if (this->upper_->stat(path))
        {
            return this->upper_->remove(path);
        }

        return STATUS_SUCCESS;
    }

    std::optional<std::filesystem::path> layered_provider::get_host_path(const windows_path& path) const
    {
        if (!this->upper_->stat(path) && this->lower_stat(path))
        {
            return this->lower_->get_host_path(path);
        }

        return this->upper_->get_host_path(path);
    }

} // namespace sogen
