#pragma once

namespace sogen
{
    struct handle;
    struct io_completion;
    struct io_completion_message;
    class windows_emulator;

    namespace worker_factory_support
    {
        bool enqueue_release_completion(windows_emulator& win_emu, handle worker_factory_handle);
        void on_io_completion_message_dequeued(windows_emulator& win_emu, const io_completion_message& message);
        // The end of NtWaitForWorkViaWorkerFactory for a worker that took work: another worker starts for work still
        // queued with none free to take it, or a factory with bindings waits for its port's next packet.
        void check_for_more_work(windows_emulator& win_emu, handle worker_factory_handle);
        // ExpWorkerFactoryManagerThread: a packet queued on a port starts a worker for a factory that waited for one.
        void on_io_completion_queued(windows_emulator& win_emu, const io_completion& completion);
    }
} // namespace sogen
