#pragma once

#include "memory_permission_ext.hpp"

#include <memory_region.hpp>

#include <compare>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <cstdint>

#include <utils/serialization.hpp>
#include <utils/shared_host_buffer.hpp>
#include <utils/stat.hpp>

namespace sogen
{
    class memory_manager;

    // Section memory whose views each occupy only their own guest range, as on Windows. Every view
    // aliases the section's host memory, so views stay coherent while address space is only spent on what is mapped.
    // 32-bit callers rely on that: DXVK's D3D9 allocator keeps its texture memory in sections and maps small views on
    // demand to stay within 4 GiB.
    class section_view_manager
    {
      public:
        struct file_stamp
        {
            uint64_t size{};
            int64_t write_time{};

            bool operator==(const file_stamp&) const = default;
        };

        // The host file by its volume and file index, which a rename keeps.
        struct file_id
        {
            uint64_t device{};
            uint64_t index{};

            auto operator<=>(const file_id&) const = default;
        };

        static file_id get_file_id(const struct compat_stat& info);
        static file_stamp get_file_stamp(const struct compat_stat& info);

        // Windows keeps the pages of a file whose last view is gone cached for the file's next section; host memory bounds
        // how much stays here.
        static constexpr uint64_t unused_file_storage_limit = 512ULL * 1024 * 1024;

        // The new storage holds one reference, owned by the section object. Shareable storage can be opened by another
        // process through its memory's name.
        uint64_t create_storage(size_t size, bool shareable = false);
        uint64_t adopt_storage(utils::shared_host_buffer memory);

        // One storage per file serves the views of all its data sections, as Windows maps them through the file's one
        // control area.
        uint64_t adopt_file_storage(utils::shared_host_buffer memory, file_id file, std::u16string file_name, file_stamp stamp);
        uint64_t adopt_private_copy(utils::shared_host_buffer memory, std::u16string file_name);
        uint64_t find_file_storage(const file_id& file) const;
        // Host processes can change the file while no view maps it, so an unused storage is only taken while the file keeps
        // the stamp it was read with.
        uint64_t take_file_storage(const file_id& file, const file_stamp& stamp, size_t minimum_size);
        void grow_storage(memory_manager& memory, uint64_t storage_id, utils::shared_host_buffer larger);

        bool has_file_storages() const
        {
            return !this->file_storages_.empty();
        }

        void write_file_storage(const file_id& file, uint64_t offset, std::span<const std::byte> data, const file_stamp& stamp);
        void add_reference(uint64_t storage_id);
        void release_storage(uint64_t storage_id);
        size_t storage_size(uint64_t storage_id) const;
        const utils::shared_host_buffer* storage_memory(uint64_t storage_id) const;

        // Maps the view at address, or in place of a placeholder there, when it is nonzero.
        uint64_t map_view(memory_manager& memory, uint64_t storage_id, uint64_t offset, size_t size, nt_memory_permission permissions,
                          uint64_t highest_address, uint64_t address = 0);
        bool unmap_view(memory_manager& memory, uint64_t address, bool preserve_placeholder = false);
        bool contains(uint64_t address, uint64_t size) const;

        void serialize(utils::buffer_serializer& buffer, const memory_manager& memory) const;
        void deserialize(utils::buffer_deserializer& buffer);
        void restore_views(memory_manager& memory);

      private:
        struct storage
        {
            utils::shared_host_buffer memory{};
            uint32_t references{};
            std::optional<file_id> file{};
            std::u16string file_name{};
            file_stamp stamp{};
        };

        struct view
        {
            uint64_t storage_id{};
            uint64_t offset{};
            uint64_t size{};
            nt_memory_permission permissions{};
        };

        uint64_t next_storage_id_{1};
        std::map<uint64_t, storage> storages_{};
        std::map<file_id, uint64_t> file_storages_{};
        std::list<uint64_t> kept_storages_{};
        uint64_t kept_bytes_{};
        std::map<uint64_t, view> views_{};
        std::map<uint64_t, std::vector<basic_memory_region<nt_memory_permission>>> restored_protections_{};

        std::map<uint64_t, view>::const_iterator find_view(uint64_t address) const;

        void drop_reference(std::map<uint64_t, storage>::iterator entry);
        void erase_storage(std::map<uint64_t, storage>::iterator entry);
        void trim_kept_storages();
    };
}
