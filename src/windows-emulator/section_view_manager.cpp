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

    section_view_manager::file_id section_view_manager::get_file_id(const struct compat_stat& info)
    {
        return {.device = info.st_dev, .index = info.st_ino};
    }

    section_view_manager::file_stamp section_view_manager::get_file_stamp(const struct compat_stat& info)
    {
        const auto write_time = std::chrono::seconds{info.st_mtimespec.tv_sec} + std::chrono::nanoseconds{info.st_mtimespec.tv_nsec};
        return {
            .size = static_cast<uint64_t>(std::max<int64_t>(info.st_size, 0)),
            .write_time = std::chrono::duration_cast<std::chrono::nanoseconds>(write_time).count(),
        };
    }

    uint64_t section_view_manager::adopt_file_storage(utils::shared_host_buffer memory, const file_id file, std::u16string file_name,
                                                      const file_stamp stamp)
    {
        const auto id = this->adopt_private_copy(std::move(memory), std::move(file_name));
        auto& stored = this->storages_.at(id);
        stored.file = file;
        stored.stamp = stamp;

        const auto previous = this->storages_.find(this->find_file_storage(file));
        this->file_storages_[file] = id;
        if (previous != this->storages_.end() && previous->second.references == 0)
        {
            this->erase_storage(previous);
        }
        return id;
    }

    uint64_t section_view_manager::adopt_private_copy(utils::shared_host_buffer memory, std::u16string file_name)
    {
        const auto id = this->adopt_storage(std::move(memory));
        this->storages_.at(id).file_name = std::move(file_name);
        return id;
    }

    uint64_t section_view_manager::find_file_storage(const file_id& file) const
    {
        const auto entry = this->file_storages_.find(file);
        return entry == this->file_storages_.end() ? 0 : entry->second;
    }

    uint64_t section_view_manager::take_file_storage(const file_id& file, const file_stamp& stamp, const size_t minimum_size)
    {
        const auto entry = this->storages_.find(this->find_file_storage(file));
        if (entry == this->storages_.end())
        {
            return 0;
        }

        if (entry->second.references == 0 && (stamp != entry->second.stamp || entry->second.memory.size() < minimum_size))
        {
            this->erase_storage(entry);
            return 0;
        }

        this->add_reference(entry->first);
        return entry->first;
    }

    void section_view_manager::grow_storage(memory_manager& memory, const uint64_t storage_id, utils::shared_host_buffer larger)
    {
        auto& stored = this->storages_.at(storage_id);
        std::memcpy(larger.data(), stored.memory.data(), stored.memory.size());

        for (const auto& [address, mapped] : this->views_)
        {
            if (mapped.storage_id != storage_id)
            {
                continue;
            }

            const auto regions = memory.get_committed_regions(address);
            if (!memory.release_memory(address, 0) || !memory.allocate_host_memory_at(address, static_cast<size_t>(mapped.size),
                                                                                      larger.data() + mapped.offset, mapped.permissions))
            {
                throw std::runtime_error("Failed to move a section view to its file's larger copy");
            }

            for (const auto& region : regions)
            {
                if (region.permissions != mapped.permissions &&
                    !memory.protect_memory(region.start, region.length, region.permissions, nullptr))
                {
                    throw std::runtime_error("Failed to keep the protection of a section view on its file's larger copy");
                }
            }

            if (!stored.file_name.empty())
            {
                memory.set_region_mapped_filename(address, stored.file_name);
            }
        }

        stored.memory = std::move(larger);
    }

    void section_view_manager::write_file_storage(const file_id& file, const uint64_t offset, const std::span<const std::byte> data,
                                                  const file_stamp& stamp)
    {
        const auto entry = this->storages_.find(this->find_file_storage(file));
        if (entry == this->storages_.end())
        {
            return;
        }

        if (offset < entry->second.memory.size())
        {
            const auto size = std::min(static_cast<uint64_t>(data.size()), entry->second.memory.size() - offset);
            std::memcpy(entry->second.memory.data() + offset, data.data(), static_cast<size_t>(size));
        }
        entry->second.stamp = stamp;
    }

    void section_view_manager::add_reference(const uint64_t storage_id)
    {
        auto& stored = this->storages_.at(storage_id);
        if (stored.references++ == 0 && std::erase(this->kept_storages_, storage_id) != 0)
        {
            this->kept_bytes_ -= stored.memory.size();
        }
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

        if (!entry->second.file_name.empty())
        {
            memory.set_region_mapped_filename(address, entry->second.file_name);
        }
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
            buffer.write_optional(stored.file);
            buffer.write(stored.file_name);
            buffer.write(stored.stamp.size);
            buffer.write(stored.stamp.write_time);
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
        this->file_storages_.clear();
        this->kept_storages_.clear();
        this->kept_bytes_ = 0;

        buffer.read(this->next_storage_id_);
        const auto storage_count = buffer.read<size_t>();
        for (size_t i = 0; i < storage_count; ++i)
        {
            const auto id = buffer.read<uint64_t>();
            const auto references = buffer.read<uint32_t>();

            auto& stored = this->storages_[id];
            buffer.read_optional(stored.file);
            buffer.read(stored.file_name);
            buffer.read(stored.stamp.size);
            buffer.read(stored.stamp.write_time);
            const auto size = buffer.read<size_t>();
            stored.memory = utils::shared_host_buffer{size};
            stored.references = references;
            if (stored.file)
            {
                this->file_storages_[*stored.file] = id;
            }
            if (references == 0)
            {
                this->kept_storages_.push_back(id);
                this->kept_bytes_ += size;
            }

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

            if (!stored.file_name.empty())
            {
                memory.set_region_mapped_filename(address, stored.file_name);
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
        if (--entry->second.references != 0)
        {
            return;
        }

        const auto is_file_storage = entry->second.file && this->find_file_storage(*entry->second.file) == entry->first;
        if (!is_file_storage || entry->second.memory.size() > unused_file_storage_limit)
        {
            this->erase_storage(entry);
            return;
        }

        this->kept_storages_.push_back(entry->first);
        this->kept_bytes_ += entry->second.memory.size();
        this->trim_kept_storages();
    }

    void section_view_manager::erase_storage(const std::map<uint64_t, storage>::iterator entry)
    {
        if (std::erase(this->kept_storages_, entry->first) != 0)
        {
            this->kept_bytes_ -= entry->second.memory.size();
        }

        if (entry->second.file)
        {
            const auto file = this->file_storages_.find(*entry->second.file);
            if (file != this->file_storages_.end() && file->second == entry->first)
            {
                this->file_storages_.erase(file);
            }
        }
        this->storages_.erase(entry);
    }

    void section_view_manager::trim_kept_storages()
    {
        while (this->kept_bytes_ > unused_file_storage_limit && !this->kept_storages_.empty())
        {
            this->erase_storage(this->storages_.find(this->kept_storages_.front()));
        }
    }
}
