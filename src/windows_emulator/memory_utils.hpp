#pragma once
#include <cstdint>
#include <emulator.hpp>

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


inline memory_permission get_memory_protection(emulator& emu, const uint64_t address)
{
	for (const auto& region : emu.get_memory_regions())
	{
		if (is_within_start_and_length(address, region.start, region.length))
		{
			return region.pemissions;
		}
	}

	return memory_permission::none;
}

inline bool is_memory_allocated(emulator& emu, const uint64_t address)
{
	for (const auto& region : emu.get_memory_regions())
	{
		if (is_within_start_and_length(address, region.start, region.length))
		{
			return true;
		}
	}

	return false;
}

inline memory_permission map_nt_to_emulator_protection(const uint32_t nt_protection)
{
	switch (nt_protection)
	{
	case PAGE_NOACCESS:
		return memory_permission::none;
	case PAGE_READONLY:
		return memory_permission::read;
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
		return memory_permission::read | memory_permission::write;
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:
		return memory_permission::read | memory_permission::exec;
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
	default:
		return memory_permission::all;
	}
}

inline uint32_t map_emulator_to_nt_protection(const memory_permission permission)
{
	const bool has_exec = (permission & memory_permission::exec) != memory_permission::none;
	const bool has_read = (permission & memory_permission::read) != memory_permission::none;
	const bool has_write = (permission & memory_permission::write) != memory_permission::none;

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
