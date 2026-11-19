#pragma once

#ifdef _WIN32

#include "win.hpp"
#include <thread>

#if defined(__MINGW32__) && (defined(_GLIBCXX_GCC_GTHR_POSIX_H) || defined(_LIBCPP_HAS_THREAD_API_PTHREAD))
#include <pthread.h>
#endif

namespace sogen::utils::nt
{
    inline HANDLE get_thread_handle(std::thread& thread)
    {
#if defined(__MINGW32__) && (defined(_GLIBCXX_GCC_GTHR_POSIX_H) || defined(_LIBCPP_HAS_THREAD_API_PTHREAD))
        return pthread_gethandle(thread.native_handle());
#else
        return thread.native_handle();
#endif
    }
}

#endif
