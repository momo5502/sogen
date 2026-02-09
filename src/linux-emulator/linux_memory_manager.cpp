#include "std_include.hpp"
#include "linux_memory_manager.hpp"

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
    buffer.read(this->mmap_base_);
    buffer.read_map(this->mapped_regions_);

    std::vector<uint8_t> data{};

    for (const auto& region : this->mapped_regions_)
    {
        data.resize(region.second.length);
        buffer.read(data.data(), region.second.length);

        this->map_memory(region.first, region.second.length, region.second.permissions);
        this->write_memory(region.first, data.data(), region.second.length);
    }
}

bool linux_memory_manager::allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions)
{
    if (this->overlaps_mapped_region(address, size))
    {
        return false;
    }

    this->map_memory(address, size, permissions);
    this->mapped_regions_[address] = mapped_region{size, permissions};

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
    const auto prot_start = page_align_down(address);
    const auto prot_end = page_align_up(address + size);

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
            continue;
        }

        // Region needs splitting
        to_remove.push_back(base);

        // Part before the protected range
        if (base < prot_start)
        {
            const auto before_len = static_cast<size_t>(prot_start - base);
            to_add.push_back({base, {before_len, region.permissions}});
        }

        // The protected part
        const auto overlap_start = std::max(base, prot_start);
        const auto overlap_end = std::min(region_end, prot_end);
        const auto overlap_len = static_cast<size_t>(overlap_end - overlap_start);
        this->apply_memory_protection(overlap_start, overlap_len, permissions);
        to_add.push_back({overlap_start, {overlap_len, permissions}});

        // Part after the protected range
        if (region_end > prot_end)
        {
            const auto after_len = static_cast<size_t>(region_end - prot_end);
            to_add.push_back({prot_end, {after_len, region.permissions}});
        }
    }

    for (const auto& key : to_remove)
    {
        this->mapped_regions_.erase(key);
    }

    for (auto& [addr, region] : to_add)
    {
        this->mapped_regions_[addr] = std::move(region);
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

        this->unmap_memory(it->first, it->second.length);
        this->mapped_regions_.erase(it);
        return true;
    }

    const auto aligned_start = page_align_down(address);
    const auto aligned_end = page_align_up(address + size);

    // Collect regions to split/remove
    std::vector<std::pair<uint64_t, mapped_region>> to_add{};
    std::vector<uint64_t> to_remove{};

    for (auto it = this->mapped_regions_.begin(); it != this->mapped_regions_.end(); ++it)
    {
        const auto region_end = it->first + it->second.length;

        // No overlap
        if (it->first >= aligned_end || region_end <= aligned_start)
        {
            continue;
        }

        to_remove.push_back(it->first);

        // Region extends before the unmapped range — keep the prefix
        if (it->first < aligned_start)
        {
            const auto before_len = static_cast<size_t>(aligned_start - it->first);
            to_add.push_back({it->first, {before_len, it->second.permissions}});
        }

        // Region extends after the unmapped range — keep the suffix
        if (region_end > aligned_end)
        {
            const auto after_len = static_cast<size_t>(region_end - aligned_end);
            to_add.push_back({aligned_end, {after_len, it->second.permissions}});
        }

        // Unmap the overlapping portion
        const auto unmap_start = std::max(it->first, aligned_start);
        const auto unmap_end = std::min(region_end, aligned_end);
        this->unmap_memory(unmap_start, static_cast<size_t>(unmap_end - unmap_start));
    }

    for (const auto& key : to_remove)
    {
        this->mapped_regions_.erase(key);
    }

    for (auto& [addr, region] : to_add)
    {
        this->mapped_regions_[addr] = std::move(region);
    }

    return true;
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
