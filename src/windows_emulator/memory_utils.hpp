#pragma once
#include <cstdint>
#include <emulator.hpp>

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
