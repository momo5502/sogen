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
    }
} // namespace sogen
