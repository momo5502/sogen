#pragma once

namespace sogen
{
    struct handle;
    struct io_completion_message;
    struct process_context;

    namespace worker_factory_support
    {
        bool enqueue_release_completion(process_context& process, handle worker_factory_handle);
        void on_io_completion_message_dequeued(process_context& process, const io_completion_message& message);
    }
} // namespace sogen
