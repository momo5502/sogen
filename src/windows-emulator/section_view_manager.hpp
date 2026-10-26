#pragma once

#include "memory_permission_ext.hpp"

#include <map>
#include <cstdint>

#include <utils/serialization.hpp>
#include <utils/shared_host_buffer.hpp>

namespace sogen
{
    class memory_manager;

    // Pagefile-backed section memory whose views each occupy only their own guest range, as on Windows. Every view
    // aliases the section's host memory, so views stay coherent while address space is only spent on what is mapped.
    // 32-bit callers rely on that: DXVK's D3D9 allocator keeps its texture memory in sections and maps small views on
    // demand to stay within 4 GiB.
    class section_view_manager
    {
      public:
        // The new storage holds one reference, owned by the section object.
        uint64_t create_storage(size_t size);
        void release_storage(uint64_t storage_id);
        size_t storage_size(uint64_t storage_id) const;

        uint64_t map_view(memory_manager& memory, uint64_t storage_id, uint64_t offset, size_t size, nt_memory_permission permissions,
                          uint64_t highest_address);
        bool unmap_view(memory_manager& memory, uint64_t address);

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
        // After a restore, maps every view back at its saved address.
        void restore_views(memory_manager& memory) const;

      private:
        struct storage
        {
            utils::shared_host_buffer memory{};
            uint32_t references{};
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
        std::map<uint64_t, view> views_{};

        void drop_reference(std::map<uint64_t, storage>::iterator entry);
    };
}
