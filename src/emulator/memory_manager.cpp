#include "memory_manager.hpp"

#include "memory_region.hpp"
#include "address_utils.hpp"

#include <vector>
#include <optional>
#include <stdexcept>
#include <cassert>

namespace
{
	constexpr auto MIN_ALLOCATION_ADDRESS = 0x0000000000010000ULL;
	constexpr auto MAX_ALLOCATION_ADDRESS = 0x00007ffffffeffffULL;

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

					i->second.length = first_length;

					regions[split_point] = memory_manager::committed_region{second_length, i->second.pemissions};
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

			if (end != next->first || i->second.pemissions != next->second.pemissions)
			{
				++i;
				continue;
			}

			i->second.length += next->second.length;
			regions.erase(next);
		}
	}
}

bool memory_manager::protect_memory(const uint64_t address, const size_t size, const memory_permission permissions,
                                    memory_permission* old_permissions)
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
				old_first_permissions = sub_region.second.pemissions;
			}

			this->apply_memory_protection(sub_region.first, sub_region.second.length, permissions);
			sub_region.second.pemissions = permissions;
		}
	}

	if (old_permissions)
	{
		*old_permissions = old_first_permissions.value_or(memory_permission::none);
	}

	merge_regions(committed_regions);
	return true;
}

bool memory_manager::allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions,
                                     const bool reserve_only)
{
	if (this->overlaps_reserved_region(address, size))
	{
		return false;
	}

	const auto entry = this->reserved_regions_.try_emplace(address, size).first;

	if (!reserve_only)
	{
		this->map_memory(address, size, permissions);
		entry->second.committed_regions[address] = committed_region{size, permissions};
	}

	return true;
}

bool memory_manager::commit_memory(const uint64_t address, const size_t size, const memory_permission permissions)
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
				this->map_memory(map_start, map_length, permissions);
				committed_regions[map_start] = committed_region{map_length, permissions};
			}

			last_region_start = sub_region.first;
			last_region = &sub_region.second;
		}
	}

	if (!last_region || (last_region_start + last_region->length) < end)
	{
		const auto map_start = last_region ? (last_region_start + last_region->length) : address;
		const auto map_length = end - map_start;

		this->map_memory(map_start, map_length, permissions);
		committed_regions[map_start] = committed_region{map_length, permissions};
	}

	merge_regions(committed_regions);
	return true;
}

bool memory_manager::decommit_memory(const uint64_t address, const size_t size)
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
	return true;
}

uint64_t memory_manager::find_free_allocation_base(const size_t size, const uint64_t start) const
{
	uint64_t start_address =
		std::max(MIN_ALLOCATION_ADDRESS, start ? start : 0x100000000ULL);

	for (const auto& region : this->reserved_regions_)
	{
		const auto region_end = region.first + region.second.length;
		if(region_end < start_address)
		{
			continue;
		}

		if (!regions_with_length_intersect(start_address, size, region.first, region.second.length))
		{
			return start_address;
		}

		start_address = page_align_up(region_end);
	}

	if (start_address + size <= MAX_ALLOCATION_ADDRESS)
	{
		return start_address;
	}

	return 0;
}

region_info memory_manager::get_region_info(const uint64_t address)
{
	region_info result{};
	result.start = MIN_ALLOCATION_ADDRESS;
	result.length = MAX_ALLOCATION_ADDRESS - result.start;
	result.pemissions = memory_permission::none;
	result.allocation_base = {};
	result.is_committed = false;
	result.is_reserved = false;

	if (this->reserved_regions_.empty())
	{
		return result;
	}

	auto upper_bound = this->reserved_regions_.upper_bound(address);
	if (upper_bound == this->reserved_regions_.begin())
	{
		result.length = upper_bound->first - result.start;
		return result;
	}

	const auto entry = --upper_bound;
	const auto lower_end = entry->first + entry->second.length;
	if (lower_end <= address)
	{
		result.start = lower_end;
		result.length = MAX_ALLOCATION_ADDRESS - result.start;
		return result;
	}

	// We have a reserved region
	const auto& reserved_region = entry->second;
	const auto& committed_regions = reserved_region.committed_regions;

	result.is_reserved = true;
	result.allocation_base = entry->first;
	result.start = result.allocation_base;
	result.length = reserved_region.length;

	if (committed_regions.empty())
	{
		return result;
	}

	auto committed_bound = committed_regions.upper_bound(address);
	if (committed_bound == committed_regions.begin())
	{
		result.length = committed_bound->first - result.start;
		return result;
	}

	const auto committed_entry = --committed_bound;
	const auto committed_lower_end = committed_entry->first + committed_entry->second.length;
	if (committed_lower_end <= address)
	{
		result.start = committed_lower_end;
		result.length = lower_end - result.start;
		return result;
	}

	result.is_committed = true;
	result.start = committed_entry->first;
	result.length = committed_entry->second.length;
	result.pemissions = committed_entry->second.pemissions;

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
