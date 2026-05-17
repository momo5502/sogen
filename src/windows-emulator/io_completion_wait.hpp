#pragma once

#include "process_context.hpp"

namespace io_completion_wait
{
    bool is_wait_completion_target_type(handle target_object_handle);

    bool retain_handle_reference(process_context& process, handle source_handle, handle& retained_handle);
    void release_handle_reference(process_context& process, handle& retained_handle);

    void cleanup_wait_packet_on_close(process_context& process, handle wait_packet_handle);
    void clear_wait_packet_completion_state(process_context& process, handle wait_packet_handle);
    void materialize_signaled_wait_packets(process_context& process, handle io_completion_handle);

    bool dequeue_io_completion_message(process_context& process, handle io_completion_handle, io_completion_message& out_message);
    ULONG dequeue_io_completion_entries(process_context& process, handle io_completion_handle,
                                        emulator_object<FILE_IO_COMPLETION_INFORMATION<EmulatorTraits<Emu64>>> out_entries,
                                        ULONG max_count);
}
