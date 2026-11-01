#pragma once

#include "mount_provider.hpp"

#include <unordered_map>
#include <unordered_set>

namespace sogen
{

    // Serializes the processes that share one upper. Each holder sees the others' finished changes once it holds the lock.
    class layer_lock
    {
      public:
        explicit layer_lock(const std::filesystem::path& file);
        ~layer_lock();

        layer_lock(const layer_lock&) = delete;
        layer_lock& operator=(const layer_lock&) = delete;
        layer_lock(layer_lock&&) = delete;
        layer_lock& operator=(layer_lock&&) = delete;

        class guard
        {
          public:
            explicit guard(layer_lock& lock);
            ~guard();

            guard(const guard&) = delete;
            guard& operator=(const guard&) = delete;
            guard(guard&&) = delete;
            guard& operator=(guard&&) = delete;

          private:
            layer_lock* lock_{};
        };

      private:
#ifdef OS_WINDOWS
        void* handle_{};
#else
        int fd_{-1};
#endif
    };

    // An append-only log shared by every process that uses the upper. Appends happen under the layer lock; readers pick up
    // what other processes appended since their last read.
    class layer_journal
    {
      public:
        explicit layer_journal(std::filesystem::path file);

        // Reads the whole journal and cuts off a record a crash left unfinished. Only call it under the layer lock.
        void load();
        void refresh();

        bool is_hidden(const windows_path& path) const;
        bool hides_lower_children(const windows_path& path) const;
        uint64_t generation(const windows_path& path) const;

        bool hide(const windows_path& path);
        bool hide_lower_children(const windows_path& path);
        bool advance_generations(std::initializer_list<windows_path> paths);

      private:
        bool append(const std::string& records);
        size_t apply(std::string_view records);

        std::filesystem::path file_{};
        uint64_t offset_{};
        std::unordered_set<windows_path> hidden_{};
        std::unordered_set<windows_path> opaque_{};
        std::unordered_map<windows_path, uint64_t> generations_{};
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
    // The state directory must be on the same filesystem as the upper directory, which the constructor verifies, because
    // copies are staged there and published by rename.
    //
    // A handle opened on a lower file follows the path until it copies up. Removing, renaming or creating that path
    // invalidates the handle, so a later write through it fails instead of resurrecting the old name or modifying an
    // unrelated file that now has the same path. This holds across processes, since the journal records it.
    //
    // Several processes may use the same upper and state directory. Every change to the layer runs under a lock file in
    // the state directory and starts from the journal as the other processes left it. A handle that read the lower before
    // another process copied the file up keeps reading the lower until it writes.
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

        open_result open_for_write(const windows_path& path, const std::u16string& mode);
        NTSTATUS create_directory_unchecked(const windows_path& path);
        NTSTATUS rename_unchecked(const windows_path& from, const windows_path& to);
        NTSTATUS remove_unchecked(const windows_path& path);
        bool lower_visible(const windows_path& path) const;
        std::optional<struct compat_stat> lower_stat(const windows_path& path) const;
        NTSTATUS create_upper_parents(const windows_path& path);
        bool copy_to_upper(const windows_path& source, const windows_path& destination);
        bool copy_up(const windows_path& path);

        std::unique_ptr<mount_provider> lower_{};
        std::unique_ptr<mount_provider> upper_{};
        std::filesystem::path temporary_directory_{};
        layer_lock lock_;
        mutable layer_journal journal_;
        uint64_t temporary_counter_{};
    };

} // namespace sogen
