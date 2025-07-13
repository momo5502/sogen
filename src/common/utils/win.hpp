#pragma once

#ifdef _WIN32

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef _CRT_NO_POSIX_ERROR_CODES
#define _CRT_NO_POSIX_ERROR_CODES
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef __MINGW64__
#include <windows.h>
#else
#include <Windows.h>
#endif

#endif
