#pragma once
#include <cstdint>
#include <utils/finally.hpp>

inline bool is_within_start_and_end(const uint64_t value, const uint64_t start, const uint64_t end)
{
	return value >= start && value < end;
}

inline bool is_within_start_and_length(const uint64_t value, const uint64_t start, const uint64_t length)
{
	return is_within_start_and_end(value, start, start + length);
}

inline uint64_t align_down(const uint64_t value, const uint64_t alignment)
{
	return value & ~(alignment - 1);
}

inline uint64_t align_up(const uint64_t value, const uint64_t alignment)
{
	return align_down(value + (alignment - 1), alignment);
}

inline uint64_t page_align_down(const uint64_t value)
{
	return align_down(value, 0x1000);
}

inline uint64_t page_align_up(const uint64_t value)
{
	return align_up(value, 0x1000);
}

inline uint32_t get_memory_protection(const unicorn& uc, uint64_t address)
{
	uint32_t count{};
	uc_mem_region* regions{};

	e(uc_mem_regions(uc, &regions, &count));
	const auto _ = utils::finally([&]
		{
			uc_free(regions);
		});

	for (const auto& region : std::span(regions, count))
	{
		if (is_within_start_and_end(address, region.begin, region.end))
		{
			return region.perms;
		}
	}

	return UC_PROT_NONE;
}

inline uint32_t map_nt_to_unicorn_protection(const uint32_t nt_protection)
{
	switch (nt_protection)
	{
	case PAGE_NOACCESS:
		return UC_PROT_NONE;
	case PAGE_READONLY:
		return UC_PROT_READ;
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
		return UC_PROT_READ | UC_PROT_WRITE;
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
		return UC_PROT_READ | UC_PROT_EXEC;
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
	default:
		return UC_PROT_ALL;
	}
}

inline uint32_t map_unicorn_to_nt_protection(const uint32_t unicorn_protection)
{
	const bool has_exec = unicorn_protection & UC_PROT_EXEC;
	const bool has_read = unicorn_protection & UC_PROT_READ;
	const bool has_write = unicorn_protection & UC_PROT_WRITE;

	if (!has_read)
	{
		return PAGE_NOACCESS;
	}

	if (has_exec && has_write)
	{
		return PAGE_EXECUTE_READWRITE;
	}

	if (has_exec)
	{
		return PAGE_EXECUTE_READ;
	}

	if (has_write)
	{
		return PAGE_READWRITE;
	}

	return PAGE_READONLY;
}