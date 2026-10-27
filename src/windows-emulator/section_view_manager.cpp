#include "std_include.hpp"
#include "section_view_manager.hpp"
#include "memory_manager.hpp"

namespace sogen
{
    namespace
    {
        constexpr size_t snapshot_block_size = 0x10000;
    }

    uint64_t section_view_manager::create_storage(const size_t size)
    {
        const auto id = this->next_storage_id_++;
        this->storages_.emplace(id, storage{
                                        .memory = utils::shared_host_buffer{size},
                                        .references = 1,
                                    });
        return id;
    }

    void section_view_manager::release_storage(const uint64_t storage_id)
    {
        const auto entry = this->storages_.find(storage_id);
        if (entry != this->storages_.end())
        {
            this->drop_reference(entry);
        }
    }

    size_t section_view_manager::storage_size(const uint64_t storage_id) const
    {
        const auto entry = this->storages_.find(storage_id);
        return entry == this->storages_.end() ? 0 : entry->second.memory.size();
    }

    uint64_t section_view_manager::map_view(memory_manager& memory, const uint64_t storage_id, const uint64_t offset, const size_t size,
                                            const nt_memory_permission permissions, const uint64_t highest_address)
    {
        const auto entry = this->storages_.find(storage_id);
        if (entry == this->storages_.end() || size == 0 || offset > entry->second.memory.size() ||
            size > entry->second.memory.size() - offset)
        {
            return 0;
        }

        const auto address = memory.allocate_host_memory(size, entry->second.memory.data() + offset, permissions, highest_address);
        if (address == 0)
        {
            return 0;
        }

        try
        {
            this->views_.emplace(address, view{
                                              .storage_id = storage_id,
                                              .offset = offset,
                                              .size = size,
                                              .permissions = permissions,
                                          });
        }
        catch (...)
        {
            memory.release_memory(address, 0);
            throw;
        }
        ++entry->second.references;
        return address;
    }

    bool section_view_manager::unmap_view(memory_manager& memory, const uint64_t address)
    {
        auto entry = this->views_.upper_bound(address);
        if (entry == this->views_.begin())
        {
            return false;
        }

        --entry;
        const auto [base, mapped] = *entry;
        if (address - base >= mapped.size)
        {
            return false;
        }

        if (!memory.release_memory(base, 0))
        {
            throw std::runtime_error("Failed to release a section view");
        }
        this->views_.erase(entry);
        this->release_storage(mapped.storage_id);
        return true;
    }

    void section_view_manager::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->next_storage_id_);
        buffer.write(this->storages_.size());
        for (const auto& [id, stored] : this->storages_)
        {
            buffer.write(id);
            buffer.write(stored.references);
            buffer.write(stored.memory.size());

            std::vector<uint64_t> written_blocks{};
            for (size_t offset = 0; offset < stored.memory.size(); offset += snapshot_block_size)
            {
                const auto block = std::span(stored.memory.data() + offset, std::min(snapshot_block_size, stored.memory.size() - offset));
                if (std::ranges::any_of(block, [](const uint8_t value) { return value != 0; }))
                {
                    written_blocks.push_back(offset);
                }
            }

            buffer.write_vector(written_blocks);
            for (const auto offset : written_blocks)
            {
                buffer.write(stored.memory.data() + offset, std::min(snapshot_block_size, stored.memory.size() - offset));
            }
        }

        buffer.write_map(this->views_);
    }

    void section_view_manager::deserialize(utils::buffer_deserializer& buffer)
    {
        this->views_.clear();
        this->storages_.clear();

        buffer.read(this->next_storage_id_);
        const auto storage_count = buffer.read<size_t>();
        for (size_t i = 0; i < storage_count; ++i)
        {
            const auto id = buffer.read<uint64_t>();
            const auto references = buffer.read<uint32_t>();
            const auto size = buffer.read<size_t>();

            auto& stored = this->storages_[id];
            stored.memory = utils::shared_host_buffer{size};
            stored.references = references;

            for (const auto offset : buffer.read_vector<uint64_t>())
            {
                if (offset >= size)
                {
                    throw std::runtime_error("Saved section memory block lies outside its section");
                }
                buffer.read(stored.memory.data() + offset, std::min(snapshot_block_size, static_cast<size_t>(size - offset)));
            }
        }

        buffer.read_map(this->views_);

        for (const auto& mapped : this->views_ | std::views::values)
        {
            const auto stored = this->storages_.find(mapped.storage_id);
            if (stored == this->storages_.end() || mapped.size == 0 || mapped.offset > stored->second.memory.size() ||
                mapped.size > stored->second.memory.size() - mapped.offset)
            {
                throw std::runtime_error("Section view does not fit its saved section memory");
            }
        }
    }

    void section_view_manager::restore_views(memory_manager& memory) const
    {
        for (const auto& [address, mapped] : this->views_)
        {
            const auto& stored = this->storages_.at(mapped.storage_id);
            if (!memory.allocate_host_memory_at(address, static_cast<size_t>(mapped.size), stored.memory.data() + mapped.offset,
                                                mapped.permissions))
            {
                throw std::runtime_error("Cannot restore a section view at its saved address");
            }
        }
    }

    void section_view_manager::drop_reference(const std::map<uint64_t, storage>::iterator entry)
    {
        if (--entry->second.references == 0)
        {
            this->storages_.erase(entry);
        }
    }
}
