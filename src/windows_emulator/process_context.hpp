#pragma once
#include "emulator_utils.hpp"
#include "handles.hpp"

#include "module/module_manager.hpp"

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

struct file
{
	std::wstring name{};
};

struct process_context
{
	uint64_t executed_instructions{0};
	emulator_object<TEB> teb{};
	emulator_object<PEB> peb{};
	emulator_object<RTL_USER_PROCESS_PARAMETERS> process_params{};
	emulator_object<KUSER_SHARED_DATA> kusd{};

	module_manager module_manager{};

	mapped_module* executable{};
	mapped_module* ntdll{};

	handle_store<handle_types::event, event> events{};
	handle_store<handle_types::file, file> files{};
	emulator_allocator gs_segment{};

	bool verbose{false};
};
