#include "host_process.hpp"

#include <utility>

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
        return host_process_watch{process_id}.alive();
    }

    host_process_watch::host_process_watch(const uint32_t process_id)
        : process_id_(process_id)
    {
#ifdef _WIN32
        if (process_id != 0 && process_id != current_host_process_id())
        {
            this->handle_ = ::OpenProcess(SYNCHRONIZE, FALSE, process_id);
            this->opened_ = this->handle_ != nullptr;
            this->open_error_ = this->opened_ ? 0u : static_cast<uint32_t>(::GetLastError());
        }
#endif
    }

    host_process_watch::~host_process_watch()
    {
        this->release();
    }

    host_process_watch::host_process_watch(host_process_watch&& obj) noexcept
    {
        *this = std::move(obj);
    }

    host_process_watch& host_process_watch::operator=(host_process_watch&& obj) noexcept
    {
        if (this != &obj)
        {
            this->release();
            this->process_id_ = std::exchange(obj.process_id_, 0);
            this->handle_ = std::exchange(obj.handle_, nullptr);
            this->opened_ = std::exchange(obj.opened_, false);
            this->open_error_ = std::exchange(obj.open_error_, 0);
        }

        return *this;
    }

    void host_process_watch::release()
    {
#ifdef _WIN32
        if (this->handle_)
        {
            ::CloseHandle(this->handle_);
        }
#endif
        this->handle_ = nullptr;
        this->process_id_ = 0;
        this->opened_ = false;
        this->open_error_ = 0;
    }

    bool host_process_watch::alive() const
    {
        if (this->process_id_ == 0)
        {
            return false;
        }

        if (this->process_id_ == current_host_process_id())
        {
            return true;
        }

#ifdef _WIN32
        if (!this->opened_)
        {
            // Another user's process is alive but not ours to open; only a missing one is gone for good. The error
            // is the one the open reported, not whatever the thread has set since.
            return this->open_error_ != ERROR_INVALID_PARAMETER;
        }

        return ::WaitForSingleObject(this->handle_, 0) == WAIT_TIMEOUT;
#else
        // A zombie still answers here, which is correct: it has not been reaped, so its state is not up for grabs.
        return ::kill(static_cast<pid_t>(this->process_id_), 0) == 0 || errno != ESRCH;
#endif
    }
}
