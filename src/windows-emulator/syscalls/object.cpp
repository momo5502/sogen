#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, const handle h)
    {
        const auto value = h.value;
        if (value.is_pseudo)
        {
            return STATUS_SUCCESS;
        }

        if (h.value.type == handle_types::thread)
        {
            const auto* t = c.proc.threads.get(h);
            if (t && t->ref_count == 1)
            {
                // TODO: Better handle ref counting
                return STATUS_SUCCESS;
            }
        }

        auto* handle_store = c.proc.get_handle_store(h);
        if (handle_store && handle_store->erase(h))
        {
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, const handle source_process_handle,
                                      const handle source_handle, const handle target_process_handle,
                                      const emulator_object<handle> target_handle, const ACCESS_MASK /*desired_access*/,
                                      const ULONG /*handle_attributes*/, const ULONG /*options*/)
    {
        if (source_process_handle != CURRENT_PROCESS || target_process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (source_handle.value.is_pseudo)
        {
            target_handle.write(source_handle);
            return STATUS_SUCCESS;
        }

        auto* store = c.proc.get_handle_store(source_handle);
        if (!store)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto new_handle = store->duplicate(source_handle);
        if (!new_handle)
        {
            return STATUS_INVALID_HANDLE;
        }

        target_handle.write(*new_handle);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryObject(const syscall_context& c, const handle handle,
                                  const OBJECT_INFORMATION_CLASS object_information_class,
                                  const emulator_pointer object_information, const ULONG object_information_length,
                                  const emulator_object<ULONG> return_length)
    {
        if (object_information_class == ObjectNameInformation)
        {
            if (handle.value.type != handle_types::file)
            {
                c.win_emu.log.error("Unsupported handle type for name information query: %X\n", handle.value.type);
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }

            const auto* file = c.proc.files.get(handle);
            if (!file)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto device_path = windows_path(file->name).to_device_path();

            const auto required_size = sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + (device_path.size() + 1) * 2;
            return_length.write(static_cast<ULONG>(required_size));

            if (required_size > object_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_allocator allocator(c.emu, object_information, object_information_length);
            allocator.make_unicode_string(device_path);

            return STATUS_SUCCESS;
        }

        if (object_information_class == ObjectHandleFlagInformation)
        {
            return handle_query<OBJECT_HANDLE_FLAG_INFORMATION>(c.emu, object_information, object_information_length,
                                                                return_length,
                                                                [&](OBJECT_HANDLE_FLAG_INFORMATION& info) {
                                                                    info.Inherit = 0;
                                                                    info.ProtectFromClose = 0;
                                                                });
        }

        c.win_emu.log.error("Unsupported object info class: %X\n", object_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    bool is_awaitable_object_type(const handle h)
    {
        return h.value.type == handle_types::thread       //
               || h.value.type == handle_types::mutant    //
               || h.value.type == handle_types::semaphore //
               || h.value.type == handle_types::timer     //
               || h.value.type == handle_types::event;
    }

    NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, const ULONG count,
                                             const emulator_object<handle> handles, const WAIT_TYPE wait_type,
                                             const BOOLEAN alertable, const emulator_object<LARGE_INTEGER> timeout)
    {
        if (wait_type != WaitAny && wait_type != WaitAll)
        {
            c.win_emu.log.error("Wait type not supported!\n");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        auto& t = c.win_emu.current_thread();
        t.await_objects.clear();
        t.await_any = wait_type == WaitAny;

        for (ULONG i = 0; i < count; ++i)
        {
            const auto h = handles.read(i);

            if (!is_awaitable_object_type(h))
            {
                c.win_emu.log.print(color::gray, "Unsupported handle type for NtWaitForMultipleObjects: %d!\n",
                                    h.value.type);
                return STATUS_NOT_SUPPORTED;
            }

            t.await_objects.push_back(h);
        }

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());
        }

        c.win_emu.yield_thread(alertable);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, const handle h, const BOOLEAN alertable,
                                          const emulator_object<LARGE_INTEGER> timeout)
    {
        if (!is_awaitable_object_type(h))
        {
            c.win_emu.log.print(color::gray, "Unsupported handle type for NtWaitForSingleObject: %d!\n", h.value.type);
            return STATUS_NOT_SUPPORTED;
        }

        auto& t = c.win_emu.current_thread();
        t.await_objects = {h};
        t.await_any = false;

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());
        }

        c.win_emu.yield_thread(alertable);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationObject()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
