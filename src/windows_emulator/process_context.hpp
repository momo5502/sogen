#pragma once
#include "emulator_utils.hpp"
#include "handles.hpp"

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
	uint64_t entry_point{};
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

	mapped_binary executable{};
	mapped_binary ntdll{};

	handle_store<handle_types::event, event> events{};
	handle_store<handle_types::file, file> files{};
	emulator_allocator gs_segment{};

	bool verbose{false};
};
