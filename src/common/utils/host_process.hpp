#pragma once

#include <cstdint>

namespace sogen::utils
{
    uint32_t current_host_process_id();

    // Whether the host process with that id is still running. State shared between emulator processes records the
    // holder's id so a survivor can recover it when its holder is gone, which a non-robust lock cannot do.
    bool host_process_alive(uint32_t process_id);

    // A liveness check that keeps whatever the host needs to answer it, so asking again is one call. Windows
    // otherwise needs an OpenProcess, a wait and a CloseHandle every time, which is too much for a check a waiting
    // thread reaches on every readiness pass. The retained handle also names an exact process, so a reused process
    // id cannot make a dead owner look alive.
    class host_process_watch
    {
      public:
        host_process_watch() = default;
        explicit host_process_watch(uint32_t process_id);
        ~host_process_watch();

        host_process_watch(const host_process_watch&) = delete;
        host_process_watch& operator=(const host_process_watch&) = delete;
        host_process_watch(host_process_watch&& obj) noexcept;
        host_process_watch& operator=(host_process_watch&& obj) noexcept;

        uint32_t process_id() const
        {
            return this->process_id_;
        }

        bool alive() const;

      private:
        uint32_t process_id_{};
        void* handle_{};

        void release();
    };
}
