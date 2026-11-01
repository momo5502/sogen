#include "std_include.hpp"
#include "section_view_manager.hpp"
#include "memory_manager.hpp"

namespace sogen
{
    namespace
    {
        constexpr size_t snapshot_block_size = 0x10000;
    }

    uint64_t section_view_manager::create_storage(const size_t size, const bool shareable)
    {
        return this->adopt_storage(shareable ? utils::shared_host_buffer::create_named(size) : utils::shared_host_buffer{size});
    }

    uint64_t section_view_manager::adopt_storage(utils::shared_host_buffer memory)
    {
        const auto id = this->next_storage_id_++;
        this->storages_.emplace(id, storage{
                                        .memory = std::move(memory),
                                        .references = 1,
                                    });
        return id;
    }

    void section_view_manager::add_reference(const uint64_t storage_id)
    {
        ++this->storages_.at(storage_id).references;
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
        const auto* memory = this->storage_memory(storage_id);
        return memory ? memory->size() : 0;
    }

    const utils::shared_host_buffer* section_view_manager::storage_memory(const uint64_t storage_id) const
    {
        const auto entry = this->storages_.find(storage_id);
        return entry == this->storages_.end() ? nullptr : &entry->second.memory;
    }

    uint64_t section_view_manager::map_view(memory_manager& memory, const uint64_t storage_id, const uint64_t offset, const size_t size,
                                            const nt_memory_permission permissions, const uint64_t highest_address,
                                            const uint64_t requested_address)
    {
        const auto entry = this->storages_.find(storage_id);
        if (entry == this->storages_.end() || size == 0 || offset > entry->second.memory.size() ||
            size > entry->second.memory.size() - offset)
        {
            return 0;
        }

        auto* const host_pointer = entry->second.memory.data() + offset;
        uint64_t address = requested_address;
        if (address != 0 && (address < MIN_ALLOCATION_ADDRESS || address >= MAX_ALLOCATION_END_EXCL ||
                             size > MAX_ALLOCATION_END_EXCL - address || address + size - 1 > highest_address))
        {
            return 0;
        }

        if (address == 0)
        {
            address = memory.allocate_host_memory(size, host_pointer, permissions, highest_address);
        }
        else if (memory.get_region_kind(address) == memory_region_kind::placeholder
                     ? !memory.replace_placeholder_with_host_memory(address, size, host_pointer, permissions)
                     : !memory.allocate_host_memory_at(address, size, host_pointer, permissions))
        {
            address = 0;
        }

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

    std::map<uint64_t, section_view_manager::view>::const_iterator section_view_manager::find_view(const uint64_t address) const
    {
        auto entry = this->views_.upper_bound(address);
        if (entry == this->views_.begin())
        {
            return this->views_.end();
        }

        --entry;
        return address - entry->first < entry->second.size ? entry : this->views_.end();
    }

    bool section_view_manager::contains(const uint64_t address, const uint64_t size) const
    {
        const auto entry = this->find_view(address);
        return entry != this->views_.end() && size <= entry->second.size - (address - entry->first);
    }

    bool section_view_manager::unmap_view(memory_manager& memory, const uint64_t address, const bool preserve_placeholder)
    {
        const auto entry = this->find_view(address);
        if (entry == this->views_.end())
        {
            return false;
        }

        const auto [base, mapped] = *entry;

        if (!memory.release_memory(base, 0))
        {
            throw std::runtime_error("Failed to release a section view");
        }
        this->views_.erase(entry);
        this->release_storage(mapped.storage_id);

        if (preserve_placeholder &&
            !memory.allocate_memory(base, static_cast<size_t>(mapped.size), memory_permission::none, true, memory_region_kind::placeholder))
        {
            throw std::runtime_error("Failed to leave a placeholder where a section view was");
        }
        return true;
    }

    void section_view_manager::serialize(utils::buffer_serializer& buffer, const memory_manager& memory) const
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
                buffer.write(stored.memory.data() + offset,
                             std::min(snapshot_block_size, stored.memory.size() - static_cast<size_t>(offset)));
            }
        }

        buffer.write_map(this->views_);
        for (const auto address : this->views_ | std::views::keys)
        {
            buffer.write_vector(memory.get_committed_regions(address));
        }
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
        this->restored_protections_.clear();
        for (const auto& [address, mapped] : this->views_)
        {
            auto regions = buffer.read_vector<basic_memory_region<nt_memory_permission>>();
            if (std::ranges::any_of(regions, [&](const basic_memory_region<nt_memory_permission>& region) {
                    return region.start < address || region.length == 0 || region.start - address >= mapped.size ||
                           region.length > mapped.size - (region.start - address);
                }))
            {
                throw std::runtime_error("Saved section view protection lies outside its view");
            }
            this->restored_protections_[address] = std::move(regions);
        }

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

    void section_view_manager::restore_views(memory_manager& memory)
    {
        for (const auto& [address, mapped] : this->views_)
        {
            const auto& stored = this->storages_.at(mapped.storage_id);
            if (!memory.allocate_host_memory_at(address, static_cast<size_t>(mapped.size), stored.memory.data() + mapped.offset,
                                                mapped.permissions))
            {
                throw std::runtime_error("Cannot restore a section view at its saved address");
            }

            for (const auto& region : this->restored_protections_[address])
            {
                if (region.permissions != mapped.permissions &&
                    !memory.protect_memory(region.start, region.length, region.permissions, nullptr))
                {
                    throw std::runtime_error("Cannot restore the protection of a section view");
                }
            }
        }

        this->restored_protections_.clear();
    }

    void section_view_manager::drop_reference(const std::map<uint64_t, storage>::iterator entry)
    {
        if (--entry->second.references == 0)
        {
            this->storages_.erase(entry);
        }
    }
}
