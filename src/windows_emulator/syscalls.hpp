#pragma once

#include <x64_emulator.hpp>
#include "process_context.hpp"

constexpr uint64_t PSEUDO_BIT = 1ULL << 63ULL;
constexpr uint64_t EVENT_BIT = 1ULL << 62ULL;
constexpr uint64_t DIRECTORY_BIT = 1ULL << 61ULL;
constexpr uint64_t SYMLINK_BIT = 1ULL << 60ULL;
constexpr uint64_t FILE_BIT = 1ULL << 59ULL;

constexpr uint64_t KNOWN_DLLS_DIRECTORY = DIRECTORY_BIT | PSEUDO_BIT | 0x1337;
constexpr uint64_t KNOWN_DLLS_SYMLINK = SYMLINK_BIT | PSEUDO_BIT | 0x1337;
constexpr uint64_t SHARED_SECTION = FILE_BIT | PSEUDO_BIT | 0x1337;
constexpr uint64_t CONSOLE_SERVER = FILE_BIT | PSEUDO_BIT | 0x1338;

constexpr uint64_t CONSOLE_HANDLE = FILE_BIT | PSEUDO_BIT | 0x01;
constexpr uint64_t STDOUT_HANDLE = FILE_BIT | PSEUDO_BIT | 0x02;
constexpr uint64_t STDIN_HANDLE = FILE_BIT | PSEUDO_BIT | 0x03;

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
