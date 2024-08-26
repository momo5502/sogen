#pragma once

#include <x64_emulator.hpp>
#include "process_context.hpp"

struct syscall_context;
using syscall_handler = void(*)(const syscall_context& c);

class syscall_dispatcher
{
public:
	syscall_dispatcher(const exported_symbols& ntdll_exports);

	void dispatch(x64_emulator& emu, process_context& context);

private:
	std::unordered_map<uint64_t, syscall_handler> handlers_{};
};
