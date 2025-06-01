#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtReleaseMutant(const syscall_context& c, const handle mutant_handle,
                                    const emulator_object<LONG> previous_count)
    {
        if (mutant_handle.value.type != handle_types::mutant)
        {
            c.win_emu.log.error("Bad handle type for NtReleaseMutant\n");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        auto* mutant = c.proc.mutants.get(mutant_handle);
        if (!mutant)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto [old_count, succeeded] = mutant->release(c.win_emu.current_thread().id);

        if (previous_count)
        {
            previous_count.write(static_cast<LONG>(old_count));
        }

        return succeeded ? STATUS_SUCCESS : STATUS_MUTANT_NOT_OWNED;
    }

    NTSTATUS handle_NtOpenMutant(const syscall_context& c, const emulator_object<handle> mutant_handle,
                                 const ACCESS_MASK /*desired_access*/,
                                 const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                name = read_unicode_string(
                    c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
                c.win_emu.log.print(color::dark_gray, "--> Mutant name: %s\n", u16_to_u8(name).c_str());
            }
        }

        if (name.empty())
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        for (auto& entry : c.proc.mutants)
        {
            if (entry.second.name == name)
            {
                ++entry.second.ref_count;
                mutant_handle.write(c.proc.mutants.make_handle(entry.first));
                return STATUS_SUCCESS;
            }
        }

        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS handle_NtCreateMutant(const syscall_context& c, const emulator_object<handle> mutant_handle,
                                   const ACCESS_MASK /*desired_access*/,
                                   const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                   const BOOLEAN initial_owner)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                name = read_unicode_string(c.emu, attributes.ObjectName);
                c.win_emu.log.print(color::dark_gray, "--> Mutant name: %s\n", u16_to_u8(name).c_str());
            }
        }

        if (!name.empty())
        {
            for (auto& entry : c.proc.mutants)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    mutant_handle.write(c.proc.mutants.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        mutant e{};
        e.name = std::move(name);

        if (initial_owner)
        {
            e.try_lock(c.win_emu.current_thread().id);
        }

        const auto handle = c.proc.mutants.store(std::move(e));
        mutant_handle.write(handle);

        return STATUS_SUCCESS;
    }
}
