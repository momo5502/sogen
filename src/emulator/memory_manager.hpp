#pragma once
#include <map>

#include "memory_region.hpp"
#include "address_utils.hpp"

class memory_manager
{
public:
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

	virtual ~memory_manager() = default;

	virtual void read_memory(uint64_t address, void* data, size_t size) = 0;
	virtual bool try_read_memory(uint64_t address, void* data, size_t size) = 0;
	virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

	bool protect_memory(const uint64_t address, const size_t size, const memory_permission permissions,
		memory_permission* old_permissions = nullptr);

	bool allocate_memory(const uint64_t address, const size_t size, const memory_permission permissions,
		const bool reserve_only = false);

	bool commit_memory(const uint64_t address, const size_t size, const memory_permission permissions);
	bool decommit_memory(const uint64_t address, const size_t size);

	bool release_memory(const uint64_t address, size_t size);

	uint64_t find_free_allocation_base(const size_t size) const;

private:
	using reserved_region_map = std::map<uint64_t, reserved_region>;
	reserved_region_map reserved_regions_{};

	reserved_region_map::iterator find_reserved_region(const uint64_t address);
	bool overlaps_reserved_region(const uint64_t address, const size_t size) const;

	virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void unmap_memory(uint64_t address, size_t size) = 0;

	virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;
};
