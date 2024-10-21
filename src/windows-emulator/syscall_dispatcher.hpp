#pragma once

#include "process_context.hpp"

struct syscall_context;
using syscall_handler = void(*)(const syscall_context& c);

struct syscall_handler_entry
{
	syscall_handler handler{};
	std::string name{};
};

class windows_emulator;

class syscall_dispatcher
{
public:
	syscall_dispatcher() = default;
	syscall_dispatcher(const exported_symbols& ntdll_exports, const exported_symbols& win32u_exports);

	void dispatch(windows_emulator& win_emu);

	void serialize(utils::buffer_serializer& buffer) const;
	void deserialize(utils::buffer_deserializer& buffer);

	void setup(const exported_symbols& ntdll_exports, const exported_symbols& win32u_exports);

	std::string get_syscall_name(const uint64_t id)
	{
		return this->handlers_.at(id).name;
	}

private:
	std::unordered_map<uint64_t, syscall_handler_entry> handlers_{};

	void add_handlers(std::unordered_map<std::string, syscall_handler>& handler_mapping);
	void add_handlers();
};
