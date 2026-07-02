#include "std_include.hpp"
#include "linux_memory_manager.hpp"

#include <algorithm>
#include <iterator>

namespace sogen
{

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const linux_memory_manager::mapped_region& region)
        {
            buffer.write<uint64_t>(region.length);
            buffer.write(region.permissions);
        }

        static void deserialize(buffer_deserializer& buffer, linux_memory_manager::mapped_region& region)
        {
            region.length = static_cast<size_t>(buffer.read<uint64_t>());
            region.permissions = buffer.read<memory_permission>();
        }
    }

    namespace
    {
        constexpr auto linux_private_allocation_kind = static_cast<memory_region_kind>(1);
    }

    linux_memory_region_info::linux_memory_region_info()
        : kind(linux_private_allocation_kind)
    {
    }

    namespace
    {
        linux_memory_region_info make_region_info(const uint64_t start, const linux_memory_manager::mapped_region& region)
        {
            linux_memory_region_info info{};
            info.start = start;
            info.length = region.length;
            info.permissions = region.permissions;
            info.allocation_base = start;
            info.allocation_length = region.length;
            info.is_reserved = false;
            info.is_committed = true;
            info.initial_permissions = region.permissions;
            info.kind = linux_private_allocation_kind;
            return info;
        }
    }

    void linux_memory_manager::serialize_memory_state(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->mmap_base_);
        buffer.write_map(this->mapped_regions_);

        std::vector<uint8_t> data{};

        for (const auto& region : this->mapped_regions_)
        {
            data.resize(region.second.length);
            this->read_memory(region.first, data.data(), region.second.length);
            buffer.write(data.data(), region.second.length);
        }
    }

    void linux_memory_manager::deserialize_memory_state(utils::buffer_deserializer& buffer)
    {
        const auto new_mmap_base = buffer.read<uint64_t>();
        auto new_regions = buffer.read_map<mapped_region_map>();

        std::vector<std::vector<uint8_t>> region_data{};
        region_data.reserve(new_regions.size());
        for (const auto& [_, region] : new_regions)
        {
            auto& data = region_data.emplace_back();
            data.resize(region.length);
            buffer.read(data.data(), region.length);
        }

        this->unmap_all_memory();
        this->mmap_base_ = new_mmap_base;
        this->mapped_regions_.clear();

        auto data_it = region_data.begin();
        for (const auto& [base, region] : new_regions)
        {
            this->map_memory(base, region.length, region.permissions);
            this->write_memory(base, data_it->data(), region.length);
            ++data_it;
        }

        this->mapped_regions_ = std::move(new_regions);
    }

    bool linux_memory_manager::allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        if (this->overlaps_mapped_region(address, size))
        {
            return false;
        }

        this->map_memory(address, size, permissions);
        this->mapped_regions_[address] = mapped_region{.length = size, .permissions = permissions};
        this->notify_memory_allocate(address, size, permissions, true);

        return true;
    }

    uint64_t linux_memory_manager::allocate_memory(const size_t size, const memory_permission permissions, const uint64_t start)
    {
        const auto base = this->find_free_allocation_base(size, start);
        if (!base || !this->allocate_memory(base, size, permissions))
        {
            return 0;
        }

        return base;
    }

    bool linux_memory_manager::protect_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        if (size == 0 || address > std::numeric_limits<uint64_t>::max() - size)
        {
            return false;
        }

        const auto requested_end = address + size;
        if (requested_end > std::numeric_limits<uint64_t>::max() - 0xFFF)
        {
            return false;
        }

        const auto prot_start = page_align_down(address);
        const auto prot_end = page_align_up(requested_end);

        auto covered_until = prot_start;
        while (covered_until < prot_end)
        {
            auto it = this->mapped_regions_.upper_bound(covered_until);
            if (it == this->mapped_regions_.begin())
            {
                return false;
            }

            --it;
            const auto region_end = it->first + it->second.length;
            if (it->first > covered_until || region_end <= covered_until)
            {
                return false;
            }

            covered_until = std::min(region_end, prot_end);
        }

        bool changed = false;

        // Collect regions to split (can't modify map while iterating)
        std::vector<std::pair<uint64_t, mapped_region>> to_add{};
        std::vector<uint64_t> to_remove{};

        for (auto& [base, region] : this->mapped_regions_)
        {
            const auto region_end = base + region.length;

            // No overlap
            if (base >= prot_end || region_end <= prot_start)
            {
                continue;
            }

            // Region is fully contained — just change permissions
            if (base >= prot_start && region_end <= prot_end)
            {
                this->apply_memory_protection(base, region.length, permissions);
                region.permissions = permissions;
                changed = true;
                continue;
            }

            // Region needs splitting
            to_remove.push_back(base);

            // Part before the protected range
            if (base < prot_start)
            {
                const auto before_len = static_cast<size_t>(prot_start - base);
                to_add.push_back({base, {.length = before_len, .permissions = region.permissions}});
            }

            // The protected part
            const auto overlap_start = std::max(base, prot_start);
            const auto overlap_end = std::min(region_end, prot_end);
            const auto overlap_len = static_cast<size_t>(overlap_end - overlap_start);
            this->apply_memory_protection(overlap_start, overlap_len, permissions);
            to_add.push_back({overlap_start, {.length = overlap_len, .permissions = permissions}});
            changed = true;

            // Part after the protected range
            if (region_end > prot_end)
            {
                const auto after_len = static_cast<size_t>(region_end - prot_end);
                to_add.push_back({prot_end, {.length = after_len, .permissions = region.permissions}});
            }
        }

        for (const auto& key : to_remove)
        {
            this->mapped_regions_.erase(key);
        }

        for (auto& [addr, region] : to_add)
        {
            this->mapped_regions_[addr] = region;
        }

        if (changed)
        {
            this->notify_memory_protect(address, size, permissions);
        }

        return true;
    }

    bool linux_memory_manager::release_memory(const uint64_t address, const size_t size)
    {
        if (size == 0)
        {
            auto it = this->mapped_regions_.find(address);
            if (it == this->mapped_regions_.end())
            {
                return false;
            }

            const auto release_start = it->first;
            const auto release_size = it->second.length;
            this->unmap_memory(release_start, release_size);
            this->mapped_regions_.erase(it);
            this->notify_memory_release(release_start, release_size);
            return true;
        }

        const auto aligned_start = page_align_down(address);
        const auto aligned_end = page_align_up(address + size);
        bool changed = false;

        // Collect regions to split/remove
        std::vector<std::pair<uint64_t, mapped_region>> to_add{};
        std::vector<uint64_t> to_remove{};

        for (const auto& [base, region] : this->mapped_regions_)
        {
            const auto region_end = base + region.length;

            // No overlap
            if (base >= aligned_end || region_end <= aligned_start)
            {
                continue;
            }

            to_remove.push_back(base);

            // Region extends before the unmapped range — keep the prefix
            if (base < aligned_start)
            {
                const auto before_len = static_cast<size_t>(aligned_start - base);
                to_add.push_back({base, {.length = before_len, .permissions = region.permissions}});
            }

            // Region extends after the unmapped range — keep the suffix
            if (region_end > aligned_end)
            {
                const auto after_len = static_cast<size_t>(region_end - aligned_end);
                to_add.push_back({aligned_end, {.length = after_len, .permissions = region.permissions}});
            }

            // Unmap the overlapping portion
            const auto unmap_start = std::max(base, aligned_start);
            const auto unmap_end = std::min(region_end, aligned_end);
            this->unmap_memory(unmap_start, static_cast<size_t>(unmap_end - unmap_start));
            changed = true;
        }

        for (const auto& key : to_remove)
        {
            this->mapped_regions_.erase(key);
        }

        for (auto& [addr, region] : to_add)
        {
            this->mapped_regions_[addr] = region;
        }

        if (changed)
        {
            this->notify_memory_release(aligned_start, static_cast<size_t>(aligned_end - aligned_start));
        }

        return true;
    }

    std::vector<linux_memory_region_info> linux_memory_manager::get_mapped_region_infos() const
    {
        std::vector<linux_memory_region_info> regions{};
        regions.reserve(this->mapped_regions_.size());

        for (const auto& [start, region] : this->mapped_regions_)
        {
            regions.push_back(make_region_info(start, region));
        }

        return regions;
    }

    std::optional<linux_memory_region_info> linux_memory_manager::get_region_info(const uint64_t address) const
    {
        const auto upper_bound = this->mapped_regions_.upper_bound(address);
        if (upper_bound == this->mapped_regions_.begin())
        {
            return std::nullopt;
        }

        const auto entry = std::prev(upper_bound);
        const auto start = entry->first;
        const auto& region = entry->second;
        if (address >= start + region.length)
        {
            return std::nullopt;
        }

        return make_region_info(start, region);
    }

    linux_memory_stats linux_memory_manager::compute_memory_stats() const
    {
        linux_memory_stats stats{};
        stats.region_count = this->mapped_regions_.size();

        for (const auto& [_, region] : this->mapped_regions_)
        {
            stats.mapped_bytes += region.length;
            if ((region.permissions & memory_permission::exec) != memory_permission::none)
            {
                stats.executable_bytes += region.length;
            }
        }

        return stats;
    }

    uint64_t linux_memory_manager::add_memory_allocate_callback(memory_allocate_callback callback)
    {
        const auto id = this->next_memory_callback_id_++;
        this->memory_allocate_callbacks_.emplace_back(id, std::move(callback));
        return id;
    }

    uint64_t linux_memory_manager::add_memory_protect_callback(memory_protect_callback callback)
    {
        const auto id = this->next_memory_callback_id_++;
        this->memory_protect_callbacks_.emplace_back(id, std::move(callback));
        return id;
    }

    uint64_t linux_memory_manager::add_memory_release_callback(memory_release_callback callback)
    {
        const auto id = this->next_memory_callback_id_++;
        this->memory_release_callbacks_.emplace_back(id, std::move(callback));
        return id;
    }

    void linux_memory_manager::remove_memory_callback(const uint64_t id)
    {
        const auto remove_id = [id](const auto& entry) { return entry.first == id; };
        std::erase_if(this->memory_allocate_callbacks_, remove_id);
        std::erase_if(this->memory_protect_callbacks_, remove_id);
        std::erase_if(this->memory_release_callbacks_, remove_id);
    }

    void linux_memory_manager::notify_memory_allocate(const uint64_t address, const size_t size, const memory_permission permissions,
                                                      const bool committed)
    {
        for (const auto& [_, callback] : this->memory_allocate_callbacks_)
        {
            callback(address, size, permissions, committed);
        }
    }

    void linux_memory_manager::notify_memory_protect(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        for (const auto& [_, callback] : this->memory_protect_callbacks_)
        {
            callback(address, size, permissions);
        }
    }

    void linux_memory_manager::notify_memory_release(const uint64_t address, const size_t size)
    {
        for (const auto& [_, callback] : this->memory_release_callbacks_)
        {
            callback(address, size);
        }
    }

    void linux_memory_manager::unmap_all_memory()
    {
        for (const auto& region : this->mapped_regions_)
        {
            this->unmap_memory(region.first, region.second.length);
        }

        this->mapped_regions_.clear();
    }

    uint64_t linux_memory_manager::find_free_allocation_base(const size_t size, const uint64_t start) const
    {
        uint64_t start_address = std::max(LINUX_MIN_MMAP_ADDRESS, start ? start : this->mmap_base_);
        start_address = align_up(start_address, LINUX_ALLOCATION_GRANULARITY);

        while (start_address + size <= LINUX_MAX_MMAP_END_EXCL)
        {
            bool conflict = false;

            for (const auto& region : this->mapped_regions_)
            {
                if (region.first + region.second.length <= start_address)
                {
                    continue;
                }

                if (region.first >= start_address + size)
                {
                    break;
                }

                conflict = true;
                start_address = align_up(region.first + region.second.length, LINUX_ALLOCATION_GRANULARITY);
                break;
            }

            if (!conflict)
            {
                return start_address;
            }
        }

        return 0;
    }

    bool linux_memory_manager::overlaps_mapped_region(const uint64_t address, const size_t size) const
    {
        for (const auto& region : this->mapped_regions_)
        {
            if (regions_with_length_intersect(address, size, region.first, region.second.length))
            {
                return true;
            }
        }

        return false;
    }

    void linux_memory_manager::read_memory(const uint64_t address, void* data, const size_t size) const
    {
        this->memory_->read_memory(address, data, size);
    }

    bool linux_memory_manager::try_read_memory(const uint64_t address, void* data, const size_t size) const
    {
        try
        {
            return this->memory_->try_read_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void linux_memory_manager::write_memory(const uint64_t address, const void* data, const size_t size)
    {
        this->memory_->write_memory(address, data, size);
    }

    bool linux_memory_manager::try_write_memory(const uint64_t address, const void* data, const size_t size)
    {
        try
        {
            return this->memory_->try_write_memory(address, data, size);
        }
        catch (...)
        {
            return false;
        }
    }

    void linux_memory_manager::map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->map_memory(address, size, permissions);
    }

    void linux_memory_manager::unmap_memory(const uint64_t address, const size_t size)
    {
        this->memory_->unmap_memory(address, size);
    }

    void linux_memory_manager::apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions)
    {
        this->memory_->apply_memory_protection(address, size, permissions);
    }

    void linux_memory_manager::map_mmio(const uint64_t address, const size_t size, std::function<void(uint64_t, void*, size_t)> read_cb,
                                        std::function<void(uint64_t, const void*, size_t)> write_cb)
    {
        this->memory_->map_mmio(address, size, std::move(read_cb), std::move(write_cb));
    }

} // namespace sogen
