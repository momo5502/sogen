#pragma once

#if defined(_WIN32) && !defined(__MINGW64__)
#pragma warning(push)
#pragma warning(disable : 4201) // nameless struct/union
#pragma warning(disable : 4702) // unreachable code
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "compiler.hpp"
#include "primitives.hpp"
#include "traits.hpp"
#include "unicode.hpp"
#include "status.hpp"
#include "process.hpp"
#include "kernel_mapped.hpp"
#include "memory.hpp"
#include "file_management.hpp"
#include "win_pefile.hpp"
#include "synchronisation.hpp"
#include "registry.hpp"
#include "network.hpp"
#include "threading.hpp"
#include "window.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32) && !defined(__MINGW64__)
#pragma warning(pop)
#endif
