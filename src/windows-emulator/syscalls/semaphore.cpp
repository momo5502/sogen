#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtOpenSemaphore(const syscall_context& c, const emulator_object<handle> semaphore_handle,
                                    const ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        if (!object_attributes)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto attributes = object_attributes.read();
        if (!attributes.ObjectName)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto name = read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
        if (name.empty())
        {
            return STATUS_INVALID_PARAMETER;
        }

        for (const auto& semaphore : c.proc.semaphores)
        {
            if (semaphore.second.name == name)
            {
                semaphore_handle.write(c.proc.semaphores.make_handle(semaphore.first));
                return STATUS_SUCCESS;
            }
        }

        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS handle_NtReleaseSemaphore(const syscall_context& c, const handle semaphore_handle, const ULONG release_count,
                                       const emulator_object<LONG> previous_count)
    {
        if (semaphore_handle.value.type != handle_types::semaphore)
        {
            c.win_emu.log.error("Bad handle type for NtReleaseSemaphore\n");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        auto* mutant = c.proc.semaphores.get(semaphore_handle);
        if (!mutant)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto [old_count, succeeded] = mutant->release(release_count);

        if (previous_count)
        {
            previous_count.write(static_cast<LONG>(old_count));
        }

        return succeeded ? STATUS_SUCCESS : STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }

    NTSTATUS handle_NtCreateSemaphore(const syscall_context& c, const emulator_object<handle> semaphore_handle,
                                      const ACCESS_MASK /*desired_access*/,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                      const ULONG initial_count, const ULONG maximum_count)
    {
        semaphore s{};
        s.current_count = initial_count;
        s.max_count = maximum_count;

        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                s.name = read_unicode_string(c.emu, attributes.ObjectName);
            }
        }

        if (!s.name.empty())
        {
            for (auto& entry : c.proc.semaphores)
            {
                if (entry.second.name == s.name)
                {
                    ++entry.second.ref_count;
                    semaphore_handle.write(c.proc.semaphores.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        const auto handle = c.proc.semaphores.store(std::move(s));
        semaphore_handle.write(handle);

        return STATUS_SUCCESS;
    }
}
