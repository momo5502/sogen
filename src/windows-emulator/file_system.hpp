#pragma once
#include "std_include.hpp"
#include "windows_path.hpp"
#include "filesystem/mount_provider.hpp"

namespace sogen
{

    class file_system
    {
      public:
        explicit file_system(const std::filesystem::path& root);

        void mount(const windows_path& guest_path, std::unique_ptr<mount_provider> provider);
        void map(const windows_path& guest_path, std::filesystem::path host_path);

        std::set<char> list_drives() const;
        uint8_t drive_type(char drive) const;

        std::optional<struct compat_stat> stat(const windows_path& path) const;
        std::vector<directory_entry> list_directory(const windows_path& path) const;

        open_result open(const windows_path& path, const std::u16string& mode);
        NTSTATUS create_directory(const windows_path& path);
        NTSTATUS rename(const windows_path& from, const windows_path& to);
        NTSTATUS remove(const windows_path& path);

        // Returns an empty path if no mount resolves the guest path.
        std::filesystem::path translate(const windows_path& path) const;

      private:
        struct resolved_path
        {
            mount_provider* provider{};
            windows_path mount_point{};
            windows_path relative{};
        };

        std::optional<resolved_path> resolve(const windows_path& path) const;

        std::unordered_map<windows_path, std::unique_ptr<mount_provider>> mounts_{};
#ifdef OS_WINDOWS
        bool host_passthrough_{false};
#endif
    };

} // namespace sogen
