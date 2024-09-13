#pragma once

#include <x64_emulator.hpp>
#include "process_context.hpp"
#include "handles.hpp"

struct syscall_context;
using syscall_handler = void(*)(const syscall_context& c);

struct syscall_handler_entry
{
	syscall_handler handler{};
	std::string name{};
};

class syscall_dispatcher
{
public:
	syscall_dispatcher() = default;
	syscall_dispatcher(const exported_symbols& ntdll_exports, const exported_symbols& win32u_exports);

	void dispatch(x64_emulator& emu, process_context& context);

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	void setup(const exported_symbols& ntdll_exports, const exported_symbols& win32u_exports);

private:
	std::unordered_map<uint64_t, syscall_handler_entry> handlers_{};

	void add_handlers();
};
