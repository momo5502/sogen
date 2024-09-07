#pragma once

#include <x64_emulator.hpp>
#include "process_context.hpp"

struct handle_types
{
	enum type : uint16_t
	{
		file,
		event,
		section,
		symlink,
		directory,
	};
};

#pragma pack(push)
#pragma pack(1)
struct handle_value
{
	uint64_t id : 32;
	uint64_t type : 16;
	uint64_t padding : 15;
	uint64_t is_pseudo : 1;
};
#pragma pack(pop)

static_assert(sizeof(handle_value) == 8);

union handle
{
	handle_value value;
	uint64_t bits;
	HANDLE h;
};

inline bool operator==(const handle& h1, const handle& h2)
{
	return h1.bits == h2.bits;
}

inline bool operator==(const handle& h1, const uint64_t& h2)
{
	return h1.bits == h2;
}

inline handle_value get_handle_value(const uint64_t h)
{
	handle hh{};
	hh.bits = h;
	return hh.value;
}

constexpr handle make_handle(const uint32_t id, const handle_types::type type, const bool is_pseudo)
{
	handle_value value{};

	value.padding = 0;
	value.id = id;
	value.type = type;
	value.is_pseudo = is_pseudo;

	return {value};
}

constexpr handle make_pseudo_handle(const uint32_t id, const handle_types::type type)
{
	return make_handle(id, type, true);
}

constexpr auto KNOWN_DLLS_DIRECTORY = make_pseudo_handle(0x1337, handle_types::directory);
constexpr auto KNOWN_DLLS_SYMLINK = make_pseudo_handle(0x1337, handle_types::symlink);
constexpr auto SHARED_SECTION = make_pseudo_handle(0x1337, handle_types::section);
constexpr auto CONSOLE_SERVER = make_pseudo_handle(0x1337, handle_types::section);

constexpr auto CONSOLE_HANDLE = make_pseudo_handle(0x1, handle_types::file);
constexpr auto STDOUT_HANDLE = make_pseudo_handle(0x2, handle_types::file);
constexpr auto STDIN_HANDLE = make_pseudo_handle(0x3, handle_types::file);

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
