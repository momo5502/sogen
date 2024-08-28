#pragma once
#include <map>
#include <vector>
#include <optional>

#include "memory_region.hpp"
#include "address_utils.hpp"

class memory_manager
{
public:
	virtual ~memory_manager() = default;

	virtual void read_memory(uint64_t address, void* data, size_t size) = 0;
	virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

	bool protect_memory(const uint64_t address, const size_t size, const memory_permission permissions,
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

	bool allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions,
	                     const bool reserve_only = false)
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

	bool commit_memory(const uint64_t address, const size_t size, const memory_permission permissions)
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

	bool decommit_memory(const uint64_t address, const size_t size)
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

	bool release_memory(const uint64_t address, size_t size)
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

	uint64_t find_free_allocation_base(const size_t size) const
	{
		uint64_t start_address = 0x0000000000010000;

		for (const auto& region : this->reserved_regions_)
		{
			if (!regions_with_length_intersect(start_address, size, region.first, region.second.length))
			{
				return start_address;
			}

			start_address = page_align_up(region.first + region.second.length);
		}

		if (start_address + size <= 0x00007ffffffeffff)
		{
			return start_address;
		}

		return 0;
	}

private:
	struct committed_region
	{
		size_t length{};
		memory_permission pemissions{};
	};

	using committed_region_map = std::map<uint64_t, committed_region>;

	struct reserved_region
	{
		size_t length{};
		committed_region_map committed_regions{};
	};

	using reserved_region_map = std::map<uint64_t, reserved_region>;
	reserved_region_map reserved_regions_{};

	reserved_region_map::iterator find_reserved_region(const uint64_t address)
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

	bool overlaps_reserved_region(const uint64_t address, const size_t size) const
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

	static void split_regions(committed_region_map& regions, const std::vector<uint64_t>& split_points)
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

					regions[split_point] = committed_region{second_length, i->second.pemissions};
				}
			}
		}
	}

	static void merge_regions(committed_region_map& regions)
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

	virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void unmap_memory(uint64_t address, size_t size) = 0;

	virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;
};
