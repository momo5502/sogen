#pragma once

#include <platform/compiler.hpp>

// Header-only interfaces have no owning DLL on Windows, but need shared RTTI identity across ELF/Mach-O images.
#ifdef OS_WINDOWS
#define EMULATOR_INTERFACE
#else
#define EMULATOR_INTERFACE EXPORT_SYMBOL
#endif
