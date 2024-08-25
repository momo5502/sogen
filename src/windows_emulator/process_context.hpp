#pragma once
#include "emulator_utils.hpp"

struct mapped_binary
{
	uint64_t image_base{};
	uint64_t size_of_image{};
	std::unordered_map<std::string, uint64_t> exports{};
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
	emulator_allocator gs_segment{};
};
