#pragma once

#include "mount_provider.hpp"

namespace sogen
{

    class host_file final : public i_file
    {
      public:
        explicit host_file(FILE* file);
        ~host_file() override;

        host_file(const host_file&) = delete;
        host_file& operator=(const host_file&) = delete;
        host_file(host_file&&) = delete;
        host_file& operator=(host_file&&) = delete;

        size_t read(std::span<std::byte> buffer) override;
        size_t write(std::span<const std::byte> buffer) override;
        bool flush() override;

        bool seek(int64_t offset, int origin) override;
        int64_t tell() override;
        int64_t size() override;
        bool resize(uint64_t size) override;

        std::optional<struct compat_stat> stat() override;

      private:
        FILE* file_{};
    };

    class host_directory_provider final : public mount_provider
    {
      public:
        explicit host_directory_provider(std::filesystem::path target);

        std::optional<struct compat_stat> stat(const windows_path& path) const override;
        std::vector<directory_entry> list(const windows_path& path) const override;
        open_result open(const windows_path& path, const std::u16string& mode) override;
        NTSTATUS create_directory(const windows_path& path) override;
        NTSTATUS rename(const windows_path& from, const windows_path& to) override;
        NTSTATUS remove(const windows_path& path) override;
        std::optional<std::filesystem::path> get_host_path(const windows_path& path) const override;

      private:
        std::filesystem::path target_{};
        bool lookup_ignores_case_{};
    };

} // namespace sogen
