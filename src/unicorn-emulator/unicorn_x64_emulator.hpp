#pragma once

#include <memory>
#include <x64_emulator.hpp>
#include "platform/platform.hpp"

#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define UNICORN_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif

namespace unicorn
{
	UNICORN_EMULATOR_DLL_STORAGE
	std::unique_ptr<x64_emulator> create_x64_emulator();
}
