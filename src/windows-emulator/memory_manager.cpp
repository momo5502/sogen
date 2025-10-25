#include "std_include.hpp"
#include "memory_manager.hpp"

#include "memory_region.hpp"
#include "address_utils.hpp"
#include "memory_permission_ext.hpp"

#include <vector>
#include <optional>
#include <stdexcept>
#include <cassert>

namespace
{
    void split_regions(memory_manager::committed_region_map& regions, const std::vector<uint64_t>& split_points)
    {
        for (auto i = regions.begin(); i != regions.end(); ++i)
        {
            for (const auto split_point : split_points)
            {
                if (is_within_start_and_length(split_point, i->first, i->second.length) && i->first != split_point)
                {
                    const auto first_length = split_point - i->first;
                    const auto second_length = i->second.length - first_length;

                    i->second.length = static_cast<size_t>(first_length);

                    regions[split_point] = memory_manager::committed_region{static_cast<size_t>(second_length), i->second.permissions};
                }
            }
        }
    }

    void merge_regions(memory_manager::committed_region_map& regions)
    {
        for (auto i = regions.begin(); i != regions.end();)
        {
            assert(i->second.length > 0);

            auto next = i;
            std::advance(next, 1);

            if (next == regions.end())
            {
                break;
            }

            assert(next->second.length > 0);

            const auto end = i->first + i->second.length;
            assert(end <= next->first);

            if (end != next->first || i->second.permissions != next->second.permissions)
            {
                ++i;
                continue;
            }

            i->second.length += next->second.length;
            regions.erase(next);
        }
    }
}

namespace utils
{
    static void serialize(buffer_serializer& buffer, const memory_manager::committed_region& region)
    {
        buffer.write<uint64_t>(region.length);
        buffer.write(region.permissions);
    }

    static void deserialize(buffer_deserializer& buffer, memory_manager::committed_region& region)
    {
        region.length = static_cast<size_t>(buffer.read<uint64_t>());
        region.permissions = buffer.read<nt_memory_permission>();
    }

    static void serialize(buffer_serializer& buffer, const memory_manager::reserved_region& region)
    {
        buffer.write(region.is_mmio);
        buffer.write(region.initial_permission);
        buffer.write<uint64_t>(region.length);
        buffer.write_map(region.committed_regions);
    }

    static void deserialize(buffer_deserializer& buffer, memory_manager::reserved_region& region)
    {
        buffer.read(region.is_mmio);
        buffer.read(region.initial_permission);
        region.length = static_cast<size_t>(buffer.read<uint64_t>());
        buffer.read_map(region.committed_regions);
    }
}

void memory_manager::update_layout_version()
{
#if MOMO_REFLECTION_LEVEL > 0
    this->layout_version_.fetch_add(1, std::memory_order_relaxed);
#endif
}

memory_stats memory_manager::compute_memory_stats() const
{
    memory_stats stats{};
    stats.reserved_memory = 0;
    stats.committed_memory = 0;

    for (const auto& reserved_region : this->reserved_regions_ | std::views::values)
    {
        stats.reserved_memory += reserved_region.length;

        for (const auto& committed_region : reserved_region.committed_regions | std::views::values)
        {
            stats.committed_memory += committed_region.length;
        }
    }

    return stats;
}

void memory_manager::serialize_memory_state(utils::buffer_serializer& buffer, const bool is_snapshot) const
{
    buffer.write_atomic(this->layout_version_);
    buffer.write_map(this->reserved_regions_);

    if (is_snapshot)
    {
        return;
    }

    std::vector<uint8_t> data{};

    for (const auto& reserved_region : this->reserved_regions_)
    {
        if (reserved_region.second.is_mmio)
        {
            continue;
        }

        for (const auto& region : reserved_region.second.committed_regions)
        {
            data.resize(region.second.length);

            this->read_memory(region.first, data.data(), region.second.length);

            buffer.write(data.data(), region.second.length);
        }
    }
}

void memory_manager::deserialize_memory_state(utils::buffer_deserializer& buffer, const bool is_snapshot)
{
    if (!is_snapshot)
    {
        assert(this->reserved_regions_.empty());
    }

    buffer.read_atomic(this->layout_version_);
    buffer.read_map(this->reserved_regions_);

    if (is_snapshot)
    {
        return;
    }

    std::vector<uint8_t> data{};

    for (auto i = this->reserved_regions_.begin(); i != this->reserved_regions_.end();)
    {
        auto& reserved_region = i->second;
        if (reserved_region.is_mmio)
        {
            i = this->reserved_regions_.erase(i);
            continue;
        }

        ++i;

        for (const auto& region : reserved_region.committed_regions)
        {
            data.resize(region.second.length);

            buffer.read(data.data(), region.second.length);

            this->map_memory(region.first, region.second.length, region.second.permissions);
            this->write_memory(region.first, data.data(), region.second.length);
        }
    }
}

bool memory_manager::protect_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                    nt_memory_permission* old_permissions)
{
    const auto entry = this->find_reserved_region(address);
    if (entry == this->reserved_regions_.end())
    {
        return false;
    }

    const auto end = address + size;
    const auto region_end = entry->first + entry->second.length;

    if (region_end < end)
    {
        throw std::runtime_error("Cross region protect not supported yet!");
    }

    std::optional<memory_permission> old_first_permissions{};

    auto& committed_regions = entry->second.committed_regions;
    split_regions(committed_regions, {address, end});

    for (auto& sub_region : committed_regions)
    {
        if (sub_region.first >= end)
        {
            break;
        }

        const auto sub_region_end = sub_region.first + sub_region.second.length;
        if (sub_region.first >= address && sub_region_end <= end)
        {
            if (!old_first_permissions.has_value())
            {
                old_first_permissions = sub_region.second.permissions;
            }

            this->apply_memory_protection(sub_region.first, sub_region.second.length, permissions);
            sub_region.second.permissions = permissions;
        }
    }

    if (old_permissions)
    {
        *old_permissions = old_first_permissions.value_or(memory_permission::none);
    }

    merge_regions(committed_regions);

    this->update_layout_version();

    return true;
}

bool memory_manager::allocate_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
{
    if (this->overlaps_reserved_region(address, size))
    {
        return false;
    }

    this->map_mmio(address, size, std::move(read_cb), std::move(write_cb));

    const auto entry = this->reserved_regions_
                           .try_emplace(address,
                                        reserved_region{
                                            .length = size,
                                            .is_mmio = true,
                                        })
                           .first;

    entry->second.committed_regions[address] = committed_region{size, memory_permission::read_write};

    this->update_layout_version();

    return true;
}

bool memory_manager::allocate_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions,
                                     const bool reserve_only)
{
    if (this->overlaps_reserved_region(address, size))
    {
        return false;
    }

    const auto entry = this->reserved_regions_
                           .try_emplace(address,
                                        reserved_region{
                                            .length = size,
                                            .initial_permission = permissions,
                                        })
                           .first;

    if (!reserve_only)
    {
        this->map_memory(address, size, permissions.is_guarded() ? memory_permission::none : permissions.common);
        entry->second.committed_regions[address] = committed_region{size, permissions};
    }

    this->update_layout_version();

    return true;
}

bool memory_manager::commit_memory(const uint64_t address, const size_t size, const nt_memory_permission permissions)
{
    const auto entry = this->find_reserved_region(address);
    if (entry == this->reserved_regions_.end())
    {
        return false;
    }

    const auto end = address + size;
    const auto region_end = entry->first + entry->second.length;

    if (region_end < end)
    {
        throw std::runtime_error("Cross region commit not supported yet!");
    }

    auto& committed_regions = entry->second.committed_regions;
    split_regions(committed_regions, {address, end});

    uint64_t last_region_start{};
    const committed_region* last_region{nullptr};

    for (auto& sub_region : committed_regions)
    {
        if (sub_region.first >= end)
        {
            break;
        }

        const auto sub_region_end = sub_region.first + sub_region.second.length;
        if (sub_region.first >= address && sub_region_end <= end)
        {
            const auto map_start = last_region ? (last_region_start + last_region->length) : address;
            const auto map_length = sub_region.first - map_start;

            if (map_length > 0)
            {
                this->map_memory(map_start, static_cast<size_t>(map_length), permissions);
                committed_regions[map_start] = committed_region{static_cast<size_t>(map_length), permissions};
            }

            last_region_start = sub_region.first;
            last_region = &sub_region.second;
        }
    }

    if (!last_region || (last_region_start + last_region->length) < end)
    {
        const auto map_start = last_region ? (last_region_start + last_region->length) : address;
        const auto map_length = end - map_start;

        this->map_memory(map_start, static_cast<size_t>(map_length), permissions);
        committed_regions[map_start] = committed_region{static_cast<size_t>(map_length), permissions};
    }

    merge_regions(committed_regions);

    this->update_layout_version();

    return true;
}

bool memory_manager::decommit_memory(const uint64_t address, const size_t size)
{
    const auto entry = this->find_reserved_region(address);
    if (entry == this->reserved_regions_.end())
    {
        return false;
    }

    if (entry->second.is_mmio)
    {
        throw std::runtime_error("Not allowed to decommit MMIO!");
    }

    const auto end = address + size;
    const auto region_end = entry->first + entry->second.length;

    if (region_end < end)
    {
        throw std::runtime_error("Cross region decommit not supported yet!");
    }

    auto& committed_regions = entry->second.committed_regions;

    split_regions(committed_regions, {address, end});

    for (auto i = committed_regions.begin(); i != committed_regions.end();)
    {
        if (i->first >= end)
        {
            break;
        }

        const auto sub_region_end = i->first + i->second.length;
        if (i->first >= address && sub_region_end <= end)
        {
            this->unmap_memory(i->first, i->second.length);
            i = committed_regions.erase(i);
            continue;
        }

        ++i;
    }

    this->update_layout_version();

    return true;
}

bool memory_manager::release_memory(const uint64_t address, size_t size)
{
    const auto entry = this->reserved_regions_.find(address);
    if (entry == this->reserved_regions_.end())
    {
        return false;
    }

    if (!size)
    {
        size = entry->second.length;
    }

    size = static_cast<size_t>(page_align_up(size));

    if (size > entry->second.length)
    {
        throw std::runtime_error("Cross region release not supported yet!");
    }

    const auto end = address + size;
    auto& committed_regions = entry->second.committed_regions;

    split_regions(committed_regions, {end});

    for (auto i = committed_regions.begin(); i != committed_regions.end();)
    {
        if (i->first >= end)
        {
            break;
        }

        const auto sub_region_end = i->first + i->second.length;
        if (i->first >= address && sub_region_end <= end)
        {
            this->unmap_memory(i->first, i->second.length);
            i = committed_regions.erase(i);
        }
        else
        {
            ++i;
        }
    }

    entry->second.length -= size;
    if (entry->second.length > 0)
    {
        this->reserved_regions_[address + size] = std::move(entry->second);
    }

    this->reserved_regions_.erase(entry);
    this->update_layout_version();
    return true;
}

void memory_manager::unmap_all_memory()
{
    for (const auto& reserved_region : this->reserved_regions_)
    {
        for (const auto& region : reserved_region.second.committed_regions)
        {
            this->unmap_memory(region.first, region.second.length);
        }
    }

    this->reserved_regions_.clear();
}

uint64_t memory_manager::allocate_memory(const size_t size, const nt_memory_permission permissions, const bool reserve_only, uint64_t start)
{
    const auto allocation_base = this->find_free_allocation_base(size, start);
    if (!allocate_memory(allocation_base, size, permissions, reserve_only))
    {
        return 0;
    }

    return allocation_base;
}

uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start) const
{
    uint64_t start_address = std::max(static_cast<uint64_t>(MIN_ALLOCATION_ADDRESS), start ? start : this->default_allocation_address_);
    start_address = align_up(start_address, ALLOCATION_GRANULARITY);

    // Since reserved_regions_ is a sorted map, we can iterate through it
    // and find gaps between regions
    while (start_address + size <= MAX_ALLOCATION_END_EXCL)
    {
        bool conflict = false;

        // Check if the proposed range [start_address, start_address+size) conflicts with any existing region
        for (const auto& region : this->reserved_regions_)
        {
            // If this region ends before our start, skip it
            if (region.first + region.second.length <= start_address)
            {
                continue;
            }

            // If this region starts after our end, we're done checking (map is sorted)
            if (region.first >= start_address + size)
            {
                break;
            }

            // Otherwise, we have a conflict
            conflict = true;
            // Move start_address past this conflicting region
            start_address = align_up(region.first + region.second.length, ALLOCATION_GRANULARITY);
            break;
        }

        // If no conflict was found, we have our address
        if (!conflict)
        {
            return start_address;
        }
    }

    return 0;
}

region_info memory_manager::get_region_info(const uint64_t address)
{
    region_info result{};
    result.start = MIN_ALLOCATION_ADDRESS;
    result.length = static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start);
    result.permissions = nt_memory_permission();
    result.initial_permissions = nt_memory_permission();
    result.allocation_base = {};
    result.allocation_length = result.length;
    result.is_committed = false;
    result.is_reserved = false;

    if (this->reserved_regions_.empty())
    {
        return result;
    }

    auto upper_bound = this->reserved_regions_.upper_bound(address);
    if (upper_bound == this->reserved_regions_.begin())
    {
        result.length = static_cast<size_t>(upper_bound->first - result.start);
        return result;
    }

    const auto entry = --upper_bound;
    const auto lower_end = entry->first + entry->second.length;
    if (lower_end <= address)
    {
        result.start = lower_end;
        result.length = static_cast<size_t>(MAX_ALLOCATION_END_EXCL - result.start);
        return result;
    }

    // We have a reserved region
    const auto& reserved_region = entry->second;
    const auto& committed_regions = reserved_region.committed_regions;

    result.is_reserved = true;
    result.allocation_base = entry->first;
    result.allocation_length = reserved_region.length;
    result.start = result.allocation_base;
    result.length = result.allocation_length;
    result.initial_permissions = entry->second.initial_permission;

    if (committed_regions.empty())
    {
        return result;
    }

    auto committed_bound = committed_regions.upper_bound(address);
    if (committed_bound == committed_regions.begin())
    {
        result.length = static_cast<size_t>(committed_bound->first - result.start);
        return result;
    }

    const auto committed_entry = --committed_bound;
    const auto committed_lower_end = committed_entry->first + committed_entry->second.length;
    if (committed_lower_end <= address)
    {
        result.start = committed_lower_end;
        result.length = static_cast<size_t>(lower_end - result.start);
        return result;
    }

    result.is_committed = true;
    result.start = committed_entry->first;
    result.length = committed_entry->second.length;
    result.permissions = committed_entry->second.permissions;

    return result;
}

memory_manager::reserved_region_map::iterator memory_manager::find_reserved_region(const uint64_t address)
{
    if (this->reserved_regions_.empty())
    {
        return this->reserved_regions_.end();
    }

    auto upper_bound = this->reserved_regions_.upper_bound(address);
    if (upper_bound == this->reserved_regions_.begin())
    {
        return this->reserved_regions_.end();
    }

    const auto entry = --upper_bound;
    if (entry->first + entry->second.length <= address)
    {
        return this->reserved_regions_.end();
    }

    return entry;
}

bool memory_manager::overlaps_reserved_region(const uint64_t address, const size_t size) const
{
    for (const auto& region : this->reserved_regions_)
    {
        if (regions_with_length_intersect(address, size, region.first, region.second.length))
        {
            return true;
        }
    }

    return false;
}

void memory_manager::read_memory(const uint64_t address, void* data, const size_t size) const
{
    this->memory_->read_memory(address, data, size);
}

bool memory_manager::try_read_memory(const uint64_t address, void* data, const size_t size) const
{
    return this->memory_->try_read_memory(address, data, size);
}

void memory_manager::write_memory(const uint64_t address, const void* data, const size_t size)
{
    this->memory_->write_memory(address, data, size);
}

void memory_manager::map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb)
{
    this->memory_->map_mmio(address, size, std::move(read_cb), std::move(write_cb));
}

void memory_manager::map_memory(const uint64_t address, const size_t size, const memory_permission permissions)
{
    this->memory_->map_memory(address, size, permissions);
}

void memory_manager::unmap_memory(const uint64_t address, const size_t size)
{
    this->memory_->unmap_memory(address, size);
}

void memory_manager::apply_memory_protection(const uint64_t address, const size_t size, const memory_permission permissions)
{
    this->memory_->apply_memory_protection(address, size, permissions);
}
