#include "guest_file.hpp"

#include "../emulator_utils.hpp"
#include "../file_system.hpp"

#include <utils/string.hpp>

namespace sogen
{

    namespace
    {
        std::u16string get_reopen_mode(const std::u16string& mode)
        {
            if (mode.starts_with(u'w'))
            {
                return u"r+b";
            }

            return mode;
        }

        void validate_writable_after_restore(const file_system& file_sys, const windows_path& path, const std::string_view saved_state)
        {
            if (!file_sys.is_writable(path))
            {
                throw std::runtime_error(path.string() + " was saved " + std::string(saved_state) + ", but its mount is not writable now");
            }
        }
    }

    guest_file::guest_file(file_system& file_sys, const windows_path& path, std::u16string mode, std::unique_ptr<i_file> io)
        : file_sys_(&file_sys),
          path_(path.lexically_normal()),
          mode_(std::move(mode)),
          io_(std::move(io))
    {
    }

    guest_file::~guest_file()
    {
        this->release();
    }

    guest_file::guest_file(guest_file&& obj) noexcept
    {
        this->operator=(std::move(obj));
    }

    guest_file& guest_file::operator=(guest_file&& obj) noexcept
    {
        if (this != &obj)
        {
            this->release();
            this->file_sys_ = std::exchange(obj.file_sys_, nullptr);
            this->path_ = std::move(obj.path_);
            this->mode_ = std::move(obj.mode_);
            this->io_ = std::move(obj.io_);
            this->rename_target_ = std::exchange(obj.rename_target_, std::nullopt);
            this->delete_on_close_ = std::exchange(obj.delete_on_close_, false);
        }

        return *this;
    }

    size_t guest_file::read(const std::span<std::byte> buffer) const
    {
        return this->io_->read(buffer);
    }

    size_t guest_file::write(const std::span<const std::byte> buffer) const
    {
        return this->io_->write(buffer);
    }

    bool guest_file::flush() const
    {
        return this->io_->flush();
    }

    bool guest_file::seek_to(const int64_t position, const int origin) const
    {
        return this->io_->seek(position, origin);
    }

    int64_t guest_file::tell() const
    {
        return this->io_->tell();
    }

    int64_t guest_file::size() const
    {
        return this->io_->size();
    }

    bool guest_file::resize(const uint64_t size) const
    {
        return this->io_->resize(size);
    }

    std::optional<struct compat_stat> guest_file::stat() const
    {
        return this->io_->stat();
    }

    void guest_file::defer_rename(const windows_path& new_path)
    {
        this->rename_target_ = new_path.lexically_normal();
    }

    void guest_file::set_delete_on_close(const bool delete_on_close)
    {
        this->delete_on_close_ = delete_on_close;
    }

    void guest_file::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->path_);
        buffer.write(this->mode_);
        buffer.write_optional(this->rename_target_);
        buffer.write(this->delete_on_close_);

        const auto has_io = this->io_ != nullptr;
        buffer.write(has_io);

        if (has_io)
        {
            buffer.write(this->io_->tell());
        }
    }

    void guest_file::deserialize(utils::buffer_deserializer& buffer)
    {
        this->io_.reset();
        this->rename_target_.reset();
        this->delete_on_close_ = false;

        this->file_sys_ = &buffer.read<file_system_wrapper>().get();

        buffer.read(this->path_);
        buffer.read(this->mode_);
        buffer.read_optional(this->rename_target_);
        buffer.read(this->delete_on_close_);

        if (this->delete_on_close_)
        {
            validate_writable_after_restore(*this->file_sys_, this->path_, "with delete-on-close set");
        }

        if (this->rename_target_)
        {
            validate_writable_after_restore(*this->file_sys_, this->path_, "with a pending rename");
            validate_writable_after_restore(*this->file_sys_, *this->rename_target_, "as a pending rename target");
        }

        if (!buffer.read<bool>())
        {
            return;
        }

        const auto position = buffer.read<int64_t>();

        if (is_write_open_mode(this->mode_))
        {
            validate_writable_after_restore(*this->file_sys_, this->path_, "with a writable handle");
        }

        auto result = this->file_sys_->open(this->path_, get_reopen_mode(this->mode_));
        if (!result.file)
        {
            throw std::runtime_error("Failed to reopen file " + this->path_.string() + ": status 0x" +
                                     utils::string::to_hex_number(static_cast<uint32_t>(result.status)));
        }

        if (!result.file->seek(position, SEEK_SET))
        {
            throw std::runtime_error("Failed to seek to serialized file position");
        }

        this->io_ = std::move(result.file);
    }

    void guest_file::release()
    {
        this->io_.reset();

        if (this->file_sys_)
        {
            if (this->delete_on_close_)
            {
                (void)this->file_sys_->remove(this->path_);
            }
            else if (this->rename_target_)
            {
                (void)this->file_sys_->rename(this->path_, *this->rename_target_);
            }
        }

        this->rename_target_.reset();
        this->delete_on_close_ = false;
    }

} // namespace sogen
