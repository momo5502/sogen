#pragma once

#if defined(_WIN32) || defined(_WIN64)
#define OS_WINDOWS
#elif defined(__APPLE__) || defined(__MACH__)
#define OS_MAC
#elif defined(__linux__)
#define OS_LINUX
#else
#error "Unsupported platform"
#endif

#ifdef OS_WINDOWS
#define EXPORT_SYMBOL     __declspec(dllexport)
#define IMPORT_SYMBOL     __declspec(dllimport)
#define NO_INLINE         __declspec(noinline)

#define DECLSPEC_ALIGN(n) __declspec(align(n))

#define RESTRICTED_POINTER

#else
#include <cstddef>

#define EXPORT_SYMBOL __attribute__((visibility("default")))
#define IMPORT_SYMBOL
#define NO_INLINE          __attribute__((noinline))

#define DECLSPEC_ALIGN(n)  alignas(n)
#define fopen_s            fopen
#define sscanf_s           sscanf

#define RESTRICTED_POINTER __restrict

#ifdef OS_MAC
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _stat64   stat
#else
#define _fseeki64 fseeko64
#define _ftelli64 ftello64
#define _stat64   stat64
#endif

#endif
