#pragma once

#define SOGEN_WINDOWS_EMULATOR_TEST_STD_INCLUDE

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <future>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#ifdef __MINGW64__
#include <unistd.h>
#endif

#include <gtest/gtest.h>
#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <syscall_utils.hpp>

#include "emulation_test_utils.hpp"
