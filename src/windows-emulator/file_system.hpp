#pragma once
#include "std_include.hpp"
#include "windows_path.hpp"
#include "filesystem/filesystem_mode.hpp"
#include "filesystem/mount_provider.hpp"

namespace sogen
{

    struct file_system_test_access;

    struct file_state
    {
        bool delete_pending{false};
        uint64_t posix_owner{0};
        uint64_t posix_owners_issued{0};

        uint64_t mark_delete_pending(const bool posix_semantics)
        {
            this->delete_pending = true;
            this->posix_owner = posix_semantics ? ++this->posix_owners_issued : 0;
            return this->posix_owner;
        }

        void cancel_delete()
        {
            this->delete_pending = false;
            this->posix_owner = 0;
        }
    };

    class file_system
    {
      public:
        // A copy-on-write root keeps each drive's writes in its own layer below root_layer, named after the lowercase drive letter.
        explicit file_system(const std::filesystem::path& root, filesystem_mode root_mode = default_root_mode,
                             const std::filesystem::path& root_layer = {});

        void mount(const windows_path& guest_path, std::unique_ptr<mount_provider> provider, filesystem_mode mode = default_mount_mode);
        void map(const windows_path& guest_path, std::filesystem::path host_path, filesystem_mode mode = default_mount_mode);

        std::set<char> list_drives() const;
        uint8_t drive_type(char drive) const;

        std::optional<struct compat_stat> stat(const windows_path& path) const;
        std::vector<directory_entry> list_directory(const windows_path& path) const;
        std::vector<std::u16string> list_directory_names(const windows_path& path) const;
        bool is_writable(const windows_path& path) const;

        open_result open(const windows_path& path, const std::u16string& mode);
        open_result open_read(const windows_path& path) const;
        NTSTATUS create_directory(const windows_path& path);
        NTSTATUS rename(const windows_path& from, const windows_path& to);
        NTSTATUS remove(const windows_path& path);

        std::shared_ptr<file_state> open_file_state(const windows_path& path);
        bool is_delete_pending(const windows_path& path) const;
        bool close_file_state(const windows_path& path, const std::shared_ptr<file_state>& state, uint64_t posix_owner);

        bool read_file(const windows_path& path, std::vector<std::byte>* data) const;
        std::vector<std::byte> read_file(const windows_path& path) const;

      private:
        friend struct file_system_test_access;

        // Returns an empty path if no mount resolves the guest path.
        std::filesystem::path translate(const windows_path& path) const;

        struct mount_entry
        {
            std::unique_ptr<mount_provider> provider{};
            filesystem_mode mode{};
        };

        struct resolved_path
        {
            mount_provider* provider{};
            filesystem_mode mode{};
            windows_path mount_point{};
            windows_path relative{};
        };

        std::optional<resolved_path> resolve(const windows_path& path) const;

        std::unordered_map<windows_path, mount_entry> mounts_{};
        std::unordered_map<windows_path, std::weak_ptr<file_state>> open_files_{};
#ifdef OS_WINDOWS
        bool host_passthrough_{false};
#endif
    };

} // namespace sogen
