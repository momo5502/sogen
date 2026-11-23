#pragma once

namespace sogen
{
    struct handle;
    struct object_reference;
    struct io_completion;
    struct io_completion_message;
    class emulator_thread;
    struct pending_io_completion_wait;
    class windows_emulator;

    namespace worker_factory_support
    {
        bool enqueue_release_completion(windows_emulator& win_emu, const object_reference& worker_factory);
        void on_io_completion_message_dequeued(windows_emulator& win_emu, const io_completion_message& message);
        // The end of NtWaitForWorkViaWorkerFactory for a worker that took work: another worker starts for work still
        // queued with none free to take it, or a factory with bindings waits for its port's next packet.
        void check_for_more_work(windows_emulator& win_emu, const object_reference& worker_factory);
        // A worker's wait for work passed its idle deadline. A worker the factory needs for its minimum keeps waiting with
        // a new deadline (false); any other leaves the factory and the wait ends by timeout (true), after which a factory
        // with bindings and no waiting worker waits for its port's next packet. A WorkerFactoryTimeout deadline just ends
        // the wait.
        bool end_worker_wait_by_timeout(windows_emulator& win_emu, emulator_thread& worker, pending_io_completion_wait& wait);
        // ExpWorkerFactoryManagerThread: a packet queued on a port starts a worker for a factory that waited for one.
        void on_io_completion_queued(windows_emulator& win_emu, const io_completion& completion);
    }
} // namespace sogen
