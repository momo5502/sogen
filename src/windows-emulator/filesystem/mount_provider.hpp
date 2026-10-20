#pragma once

#include "i_file.hpp"
#include "../windows_path.hpp"

namespace sogen
{

    struct directory_entry
    {
        std::u16string name{};
        std::u16string short_name{};
        bool is_directory{};
        std::optional<struct compat_stat> stat{};
    };

    struct open_result
    {
        std::unique_ptr<i_file> file{};
        NTSTATUS status{};
    };

    inline bool is_write_open_mode(const std::u16string_view mode)
    {
        return mode.find_first_of(u"wa+") != std::u16string_view::npos;
    }

    // Paths passed to a provider are relative to the mount point and contain no "." or ".." components.
    class mount_provider
    {
      public:
        virtual ~mount_provider() = default;

        virtual std::optional<struct compat_stat> stat(const windows_path& path) const = 0;
        virtual std::vector<directory_entry> list(const windows_path& path) const = 0;
        virtual open_result open(const windows_path& path, const std::u16string& mode) = 0;
        virtual NTSTATUS create_directory(const windows_path& path) = 0;
        virtual NTSTATUS rename(const windows_path& from, const windows_path& to) = 0;
        virtual NTSTATUS remove(const windows_path& path) = 0;

        // Read-only access to the backing host file for consumers that map host files directly, such as the image loader.
        virtual std::optional<std::filesystem::path> get_host_path(const windows_path& path) const = 0;
    };

} // namespace sogen
