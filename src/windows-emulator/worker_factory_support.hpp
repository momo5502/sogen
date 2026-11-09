#pragma once

namespace sogen
{
    struct handle;
    struct io_completion_message;
    class windows_emulator;

    namespace worker_factory_support
    {
        bool enqueue_release_completion(windows_emulator& win_emu, handle worker_factory_handle);
        void on_io_completion_message_dequeued(windows_emulator& win_emu, const io_completion_message& message);
        // ExpWorkerFactoryCheckCreate for a worker that took work: starts another worker when work is still queued and none
        // is free to take it.
        void check_create_worker(windows_emulator& win_emu, handle worker_factory_handle);
    }
} // namespace sogen
