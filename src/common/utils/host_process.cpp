#include "host_process.hpp"

#ifdef _WIN32
#include "win.hpp"
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace sogen::utils
{
    uint32_t current_host_process_id()
    {
#ifdef _WIN32
        return static_cast<uint32_t>(::GetCurrentProcessId());
#else
        return static_cast<uint32_t>(::getpid());
#endif
    }

    bool host_process_alive(const uint32_t process_id)
    {
        if (process_id == 0)
        {
            return false;
        }

        if (process_id == current_host_process_id())
        {
            return true;
        }

#ifdef _WIN32
        auto* process = ::OpenProcess(SYNCHRONIZE, FALSE, process_id);
        if (!process)
        {
            // Another user's process is alive but not ours to open; only a missing one is gone for good.
            return ::GetLastError() != ERROR_INVALID_PARAMETER;
        }

        const auto alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        ::CloseHandle(process);
        return alive;
#else
        // A zombie still answers here, which is correct: it has not been reaped, so its state is not up for grabs.
        return ::kill(static_cast<pid_t>(process_id), 0) == 0 || errno != ESRCH;
#endif
    }
}
