#pragma once

#define SOGEN_WINDOWS_EMULATOR_TEST_STD_INCLUDE

#ifdef __MINGW64__
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <windows_emulator.hpp>
#include <backend_selection.hpp>
