#pragma once

#include <memory>
#include <x64_emulator.hpp>

#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE __declspec(dllexport)
#else
#define UNICORN_EMULATOR_DLL_STORAGE __declspec(dllimport)
#endif

namespace unicorn
{
	UNICORN_EMULATOR_DLL_STORAGE
	std::unique_ptr<x64_emulator> create_x64_emulator();
}
