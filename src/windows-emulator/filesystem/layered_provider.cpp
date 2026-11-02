#include "layered_provider.hpp"
#include "host_directory_provider.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

#include <platform/compiler.hpp>
#include <platform/unicode.hpp>
#include <utils/finally.hpp>
#include <utils/string.hpp>

#ifdef OS_WINDOWS
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
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

        bool same_filesystem(const std::filesystem::path& first, const std::filesystem::path& second)
        {
#ifdef OS_WINDOWS
            const auto serial = [](const std::filesystem::path& path) -> std::optional<DWORD> {
                auto* const handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    return std::nullopt;
                }

                BY_HANDLE_FILE_INFORMATION information{};
                const auto ok = GetFileInformationByHandle(handle, &information);
                CloseHandle(handle);
                return ok ? std::optional<DWORD>{information.dwVolumeSerialNumber} : std::nullopt;
            };

            const auto left = serial(first);
            const auto right = serial(second);
            return left && right && *left == *right;
#else
            struct stat left{};
            struct stat right{};
            return ::stat(first.c_str(), &left) == 0 && ::stat(second.c_str(), &right) == 0 && left.st_dev == right.st_dev;
#endif
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

    layer_lock::layer_lock(const std::filesystem::path& file)
    {
        std::error_code ec{};
        std::filesystem::create_directories(file.parent_path(), ec);

#ifdef OS_WINDOWS
        this->handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (this->handle_ == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("Failed to open the layer lock: " + file.string());
        }
#else
        this->fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (this->fd_ < 0)
        {
            throw std::runtime_error("Failed to open the layer lock: " + file.string());
        }
#endif
    }

    layer_lock::~layer_lock()
    {
#ifdef OS_WINDOWS
        CloseHandle(this->handle_);
#else
        ::close(this->fd_);
#endif
    }

    layer_lock::guard::guard(layer_lock& lock)
        : lock_(&lock)
    {
#ifdef OS_WINDOWS
        OVERLAPPED overlapped{};
        if (!LockFileEx(this->lock_->handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped))
        {
            throw std::runtime_error("Failed to take the layer lock");
        }
#else
        while (::flock(this->lock_->fd_, LOCK_EX) != 0)
        {
            if (errno != EINTR)
            {
                throw std::runtime_error("Failed to take the layer lock");
            }
        }
#endif
    }

    layer_lock::guard::~guard()
    {
#ifdef OS_WINDOWS
        OVERLAPPED overlapped{};
        UnlockFileEx(this->lock_->handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
#else
        ::flock(this->lock_->fd_, LOCK_UN);
#endif
    }

    layer_journal::layer_journal(std::filesystem::path file)
        : file_(std::move(file))
    {
    }

    void layer_journal::load()
    {
        std::error_code status_error{};
        const auto journal_status = std::filesystem::status(this->file_, status_error);
        if (journal_status.type() != std::filesystem::file_type::not_found && status_error)
        {
            throw std::runtime_error("Failed to read the layer journal: " + this->file_.string());
        }

        this->offset_ = 0;
        this->hidden_.clear();
        this->opaque_.clear();
        this->generations_.clear();

        if (journal_status.type() != std::filesystem::file_type::regular)
        {
            return;
        }

        std::ifstream stream(this->file_, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (!stream.is_open() || stream.bad())
        {
            throw std::runtime_error("Failed to read the layer journal: " + this->file_.string());
        }

        this->offset_ = this->apply(content);

        // A record the crash cut short has no terminator. It is dropped and cut off, so that the next record does not
        // continue it.
        if (this->offset_ != content.size())
        {
            std::error_code ec{};
            std::filesystem::resize_file(this->file_, this->offset_, ec);
            if (ec)
            {
                throw std::runtime_error("Failed to recover the layer journal: " + this->file_.string());
            }
        }
    }

    bool layer_journal::refresh()
    {
        std::error_code ec{};
        const auto journal_status = std::filesystem::status(this->file_, ec);
        if (journal_status.type() == std::filesystem::file_type::not_found)
        {
            return true;
        }

        if (ec)
        {
            return false;
        }

        if (journal_status.type() != std::filesystem::file_type::regular)
        {
            return true;
        }

        const auto size = std::filesystem::file_size(this->file_, ec);
        if (ec)
        {
            return false;
        }

        if (size <= this->offset_)
        {
            return true;
        }

        std::ifstream stream(this->file_, std::ios::binary);
        if (!stream.is_open() || !stream.seekg(static_cast<std::streamoff>(this->offset_)))
        {
            return false;
        }

        const std::string content{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if (stream.bad())
        {
            return false;
        }

        this->offset_ += this->apply(content);
        return true;
    }

    size_t layer_journal::apply(const std::string_view records)
    {
        const auto last_newline = records.find_last_of('\n');
        if (last_newline == std::string_view::npos)
        {
            return 0;
        }

        std::istringstream lines(std::string{records.substr(0, last_newline + 1)});

        std::string line{};
        while (std::getline(lines, line))
        {
            if (line.size() < 3 || line[1] != '\t')
            {
                continue;
            }

            const windows_path path{std::filesystem::path{u8_to_u16(std::string_view{line}.substr(2))}};
            switch (line[0])
            {
            case 'H':
                this->hidden_.insert(path);
                break;
            case 'O':
                this->opaque_.insert(path);
                break;
            case 'G':
                ++this->generations_[path];
                break;
            default:
                break;
            }
        }

        return last_newline + 1;
    }

    bool layer_journal::is_hidden(const windows_path& path) const
    {
        return this->hidden_.contains(path);
    }

    bool layer_journal::hides_lower_children(const windows_path& path) const
    {
        return this->opaque_.contains(path);
    }

    uint64_t layer_journal::generation(const windows_path& path) const
    {
        const auto entry = this->generations_.find(path);
        return entry == this->generations_.end() ? 0 : entry->second;
    }

    bool layer_journal::hide(const windows_path& path)
    {
        return this->hidden_.contains(path) || this->append("H\t" + path.string() + '\n');
    }

    bool layer_journal::hide_lower_children(const windows_path& path)
    {
        return this->opaque_.contains(path) || this->append("O\t" + path.string() + '\n');
    }

    bool layer_journal::advance_generations(const std::initializer_list<windows_path> paths)
    {
        std::string records{};
        for (const auto& path : paths)
        {
            records += "G\t" + path.string() + '\n';
        }

        return this->append(records);
    }

    bool layer_journal::append(const std::string& records)
    {
        std::error_code ec{};
        std::filesystem::create_directories(this->file_.parent_path(), ec);

        // Only a writer that crashed leaves bytes past the last record this one read, since writers hold the layer lock.
        const auto size = std::filesystem::file_size(this->file_, ec);
        if (!ec && size > this->offset_)
        {
            std::filesystem::resize_file(this->file_, this->offset_, ec);
            if (ec)
            {
                return false;
            }
        }

        FILE* raw{};
        open_unicode(&raw, this->file_, u"ab");
        if (!raw)
        {
            return false;
        }

        const unique_stream stream{raw};
        if (fwrite(records.data(), 1, records.size(), stream.get()) != records.size() || !sync_stream(stream.get()))
        {
            return false;
        }

        this->offset_ += this->apply(records);
        return true;
    }

    class cow_file final : public i_file
    {
      public:
        cow_file(layered_provider& provider, windows_path path, std::unique_ptr<i_file> lower, std::u16string mode)
            : provider_(&provider),
              path_(std::move(path)),
              lower_(std::move(lower)),
              mode_(std::move(mode)),
              generation_(provider.journal_.generation(path_))
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
            if (position < 0)
            {
                return false;
            }

            // The copy is opened before the lock is released, so no other process can replace it under this name first.
            const layer_lock::guard guard{this->provider_->lock_};
            auto& journal = this->provider_->journal_;
            if (!journal.refresh() || journal.generation(this->path_) != this->generation_ ||
                !this->provider_->lower_visible(this->path_) || !this->provider_->copy_up(this->path_))
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
        uint64_t generation_{};
    };

    layered_provider::layered_provider(std::unique_ptr<mount_provider> lower, std::filesystem::path upper_directory,
                                       const std::filesystem::path& state_directory)
        : lower_(std::move(lower)),
          temporary_directory_(state_directory / "tmp"),
          lock_(state_directory / "lock"),
          journal_(state_directory / "journal")
    {
        std::error_code ec{};
        std::filesystem::create_directories(upper_directory, ec);
        std::filesystem::create_directories(this->temporary_directory_, ec);

        const layer_lock::guard guard{this->lock_};
        this->journal_.load();

        // Copies are only staged while the lock is held, so whatever is left was abandoned by a crash.
        for (const auto& entry : std::filesystem::directory_iterator(this->temporary_directory_, ec))
        {
            std::filesystem::remove_all(entry.path(), ec);
        }

        if (!same_filesystem(this->temporary_directory_, upper_directory))
        {
            throw std::runtime_error("The layer state directory must be on the same filesystem as the upper directory");
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
        this->journal_.refresh();
        if (auto upper = this->upper_->stat(path))
        {
            return upper;
        }

        return this->lower_stat(path);
    }

    std::vector<directory_entry> layered_provider::list(const windows_path& path) const
    {
        this->journal_.refresh();
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
        this->journal_.refresh();

        if (!is_write_open_mode(mode))
        {
            if (this->upper_->stat(path))
            {
                return this->upper_->open(path, mode);
            }

            if (!this->lower_stat(path))
            {
                return {.status = STATUS_OBJECT_NAME_NOT_FOUND};
            }

            return this->lower_->open(path, mode);
        }

        const layer_lock::guard guard{this->lock_};
        if (!this->journal_.refresh())
        {
            return {.status = STATUS_ACCESS_DENIED};
        }

        return this->open_for_write(path, mode);
    }

    open_result layered_provider::open_for_write(const windows_path& path, const std::u16string& mode)
    {
        if (this->upper_->stat(path))
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
        const layer_lock::guard guard{this->lock_};
        if (!this->journal_.refresh())
        {
            return STATUS_ACCESS_DENIED;
        }

        return this->create_directory_unchecked(path);
    }

    NTSTATUS layered_provider::create_directory_unchecked(const windows_path& path)
    {
        if (this->stat(path))
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (const auto status = this->create_upper_parents(path); status != STATUS_SUCCESS)
        {
            return status;
        }

        if (!this->journal_.advance_generations({path}))
        {
            return STATUS_ACCESS_DENIED;
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
        const layer_lock::guard guard{this->lock_};
        if (!this->journal_.refresh())
        {
            return STATUS_ACCESS_DENIED;
        }

        return this->rename_unchecked(from, to);
    }

    NTSTATUS layered_provider::rename_unchecked(const windows_path& from, const windows_path& to)
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

        if (is_directory(*source) && in_lower)
        {
            return STATUS_NOT_SAME_DEVICE;
        }

        // Replacing needs the old destination to survive a failed or interrupted rename, which has no durable record yet.
        if (!in_upper && this->stat(to))
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }

        if (!this->journal_.advance_generations({from, to}))
        {
            return STATUS_ACCESS_DENIED;
        }

        if (is_directory(*source))
        {
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
        const layer_lock::guard guard{this->lock_};
        if (!this->journal_.refresh())
        {
            return STATUS_ACCESS_DENIED;
        }

        return this->remove_unchecked(path);
    }

    NTSTATUS layered_provider::remove_unchecked(const windows_path& path)
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

        if (!this->journal_.advance_generations({path}))
        {
            return STATUS_ACCESS_DENIED;
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
        this->journal_.refresh();
        if (!this->upper_->stat(path) && this->lower_stat(path))
        {
            return this->lower_->get_host_path(path);
        }

        return this->upper_->get_host_path(path);
    }

} // namespace sogen
