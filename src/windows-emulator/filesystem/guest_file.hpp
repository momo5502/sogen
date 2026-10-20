#pragma once

#include "mount_provider.hpp"
#include "../windows_path.hpp"

namespace sogen
{

    class file_system;

    class guest_file
    {
      public:
        guest_file() = default;
        guest_file(file_system& file_sys, const windows_path& path, std::u16string mode = {}, std::unique_ptr<i_file> io = {});
        ~guest_file();

        guest_file(const guest_file&) = delete;
        guest_file& operator=(const guest_file&) = delete;

        guest_file(guest_file&& obj) noexcept;
        guest_file& operator=(guest_file&& obj) noexcept;

        [[nodiscard]] explicit operator bool() const
        {
            return this->io_ != nullptr;
        }

        const windows_path& path() const
        {
            return this->path_;
        }

        bool is_writable() const
        {
            return this->io_ != nullptr && is_write_open_mode(this->mode_);
        }

        size_t read(std::span<std::byte> buffer) const;
        size_t write(std::span<const std::byte> buffer) const;
        bool flush() const;

        bool seek_to(int64_t position, int origin = SEEK_SET) const;
        [[nodiscard]] int64_t tell() const;
        [[nodiscard]] int64_t size() const;
        bool resize(uint64_t size) const;
        [[nodiscard]] std::optional<struct compat_stat> stat() const;

        void defer_rename(const windows_path& new_path);
        void set_delete_on_close(bool delete_on_close);

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        file_system* file_sys_{};
        windows_path path_{};
        std::u16string mode_{};
        std::unique_ptr<i_file> io_{};
        std::optional<windows_path> rename_target_{};
        bool delete_on_close_{false};

        void release();
    };

} // namespace sogen
