#pragma once

#include "mount_provider.hpp"

#include <unordered_set>

namespace sogen
{

    class layer_journal
    {
      public:
        explicit layer_journal(std::filesystem::path file);

        bool is_hidden(const windows_path& path) const;
        bool hides_lower_children(const windows_path& path) const;

        bool hide(const windows_path& path);
        bool hide_lower_children(const windows_path& path);

      private:
        bool append(char kind, const windows_path& path);

        std::filesystem::path file_{};
        std::unordered_set<windows_path> hidden_{};
        std::unordered_set<windows_path> opaque_{};
    };

    // Presents a read-only lower provider with a writable upper directory on top. Writes, creates and renames land in
    // the upper; the lower is never modified. A lower file is copied up lazily, on its first write.
    //
    // Deleting a lower entry and re-creating a deleted lower directory are recorded in a journal that lives beside the
    // upper tree, so no marker file can collide with a guest file name. A journal record is made durable before the
    // change it describes becomes visible, so a crash leaves a state that some prefix of the operations would have left.
    //
    // Renaming a file that only the lower has is a copy followed by hiding the old name, so a crash in between leaves both
    // names, never neither. It cannot replace an existing destination yet and reports STATUS_OBJECT_NAME_COLLISION.
    //
    // Renaming a directory that has lower content is not supported yet and reports STATUS_NOT_SAME_DEVICE, which makes
    // callers fall back to copying it.
    class layered_provider final : public mount_provider
    {
      public:
        layered_provider(std::unique_ptr<mount_provider> lower, std::filesystem::path upper_directory,
                         const std::filesystem::path& state_directory);

        std::optional<struct compat_stat> stat(const windows_path& path) const override;
        std::vector<directory_entry> list(const windows_path& path) const override;
        open_result open(const windows_path& path, const std::u16string& mode) override;
        NTSTATUS create_directory(const windows_path& path) override;
        NTSTATUS rename(const windows_path& from, const windows_path& to) override;
        NTSTATUS remove(const windows_path& path) override;

      private:
        friend class cow_file;

        std::optional<std::filesystem::path> get_host_path(const windows_path& path) const override;

        bool lower_visible(const windows_path& path) const;
        std::optional<struct compat_stat> lower_stat(const windows_path& path) const;
        NTSTATUS create_upper_parents(const windows_path& path);
        bool copy_to_upper(const windows_path& source, const windows_path& destination);
        bool copy_up(const windows_path& path);

        std::unique_ptr<mount_provider> lower_{};
        std::unique_ptr<mount_provider> upper_{};
        std::filesystem::path temporary_directory_{};
        layer_journal journal_;
        uint64_t temporary_counter_{};
    };

} // namespace sogen
