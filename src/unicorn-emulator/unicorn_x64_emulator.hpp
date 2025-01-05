#pragma once

#include <memory>
#include <x64_emulator.hpp>
#include "platform/platform.hpp"

#ifdef WIN32
#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE EXPORT_SYMBOL
#else
#define UNICORN_EMULATOR_DLL_STORAGE IMPORT_SYMBOL
#endif
#else
#ifdef UNICORN_EMULATOR_IMPL
#define UNICORN_EMULATOR_DLL_STORAGE __attribute__((visibility("default")))
#else
#define UNICORN_EMULATOR_DLL_STORAGE
#endif
#endif

namespace unicorn
{
	UNICORN_EMULATOR_DLL_STORAGE
	std::unique_ptr<x64_emulator> create_x64_emulator();
}
