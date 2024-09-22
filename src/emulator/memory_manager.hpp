#pragma once
#include <map>

#include "memory_region.hpp"
#include "address_utils.hpp"
#include "serialization.hpp"

struct region_info : basic_memory_region
{
	uint64_t allocation_base{};
	size_t allocation_length{};
	bool is_reserved{};
	bool is_committed{};
};

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

	template <typename T>
	T read_memory(const uint64_t address)
	{
		T value{};
		this->read_memory(address, &value, sizeof(value));
		return value;
	}

	template <typename T>
	void write_memory(const uint64_t address, const T& value)
	{
		this->write_memory(address, &value, sizeof(value));
	}

	virtual void read_memory(uint64_t address, void* data, size_t size) const = 0;
	virtual bool try_read_memory(uint64_t address, void* data, size_t size) const = 0;
	virtual void write_memory(uint64_t address, const void* data, size_t size) = 0;

	bool protect_memory(uint64_t address, size_t size, memory_permission permissions,
	                    memory_permission* old_permissions = nullptr);

	bool allocate_memory(uint64_t address, size_t size, memory_permission permissions,
	                     bool reserve_only = false);

	bool commit_memory(uint64_t address, size_t size, memory_permission permissions);
	bool decommit_memory(uint64_t address, size_t size);

	bool release_memory(uint64_t address, size_t size);

	uint64_t find_free_allocation_base(size_t size, uint64_t start = 0) const;

	region_info get_region_info(uint64_t address);

	uint64_t allocate_memory(const size_t size, const memory_permission permissions, const bool reserve_only = false)
	{
		const auto allocation_base = this->find_free_allocation_base(size);
		if (!allocate_memory(allocation_base, size, permissions, reserve_only))
		{
			return 0;
		}

		return allocation_base;
	}

private:
	using reserved_region_map = std::map<uint64_t, reserved_region>;
	reserved_region_map reserved_regions_{};

	reserved_region_map::iterator find_reserved_region(uint64_t address);
	bool overlaps_reserved_region(uint64_t address, size_t size) const;

	virtual void map_memory(uint64_t address, size_t size, memory_permission permissions) = 0;
	virtual void unmap_memory(uint64_t address, size_t size) = 0;

	virtual void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) = 0;

protected:
	void serialize_memory_state(utils::buffer_serializer& buffer, bool is_snapshot) const;
	void deserialize_memory_state(utils::buffer_deserializer& buffer, bool is_snapshot);
};
