#pragma once
#include "emulator_utils.hpp"

struct exported_symbol
{
	std::string name{};
	uint64_t ordinal{};
	uint64_t rva{};
	uint64_t address{};
};

using exported_symbols = std::vector<exported_symbol>;

struct mapped_binary
{
	uint64_t image_base{};
	uint64_t size_of_image{};
	exported_symbols exports{};
};

struct event
{
	bool signaled{};
	EVENT_TYPE type{};

	bool is_signaled()
	{
		const auto res = this->signaled;

		if (this->type == SynchronizationEvent)
		{
			this->signaled = false;
		}

		return res;
	}
};

struct process_context
{
	emulator_object<TEB> teb{};
	emulator_object<PEB> peb{};
	emulator_object<RTL_USER_PROCESS_PARAMETERS> process_params{};
	emulator_object<KUSER_SHARED_DATA> kusd{};

	mapped_binary executable{};
	mapped_binary ntdll{};

	std::map<uint32_t, event> events{};
	std::map<uint32_t, HANDLE> os_handles{};
	std::map<uint32_t, std::wstring> files{};
	emulator_allocator gs_segment{};
};
