#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{

    namespace syscalls
    {
        NTSTATUS handle_NtReleaseMutant(const syscall_context& c, const handle mutant_handle, const emulator_object<LONG> previous_count)
        {
            if (mutant_handle.value.type != handle_types::mutant)
            {
                return mutant_handle.value.type == handle_types::reserved ? STATUS_INVALID_HANDLE : STATUS_OBJECT_TYPE_MISMATCH;
            }

            auto* mutant = c.proc.mutants.get(mutant_handle);
            if (!mutant)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto [old_count, succeeded] = mutant->release(mutant::make_owner(c.proc.process_id, c.thread().id));

            if (!succeeded)
            {
                return mutant->is_abandoned() ? STATUS_ABANDONED : STATUS_MUTANT_NOT_OWNED;
            }

            if (previous_count)
            {
                previous_count.write(1 - static_cast<LONG>(old_count));
            }

            if (old_count == 1)
            {
                c.win_emu.wake_idle_vcpu();
            }

            return STATUS_SUCCESS;
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
                    name = read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
                    c.win_emu.callbacks.on_generic_access("Opening mutant", name);
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
                    c.win_emu.callbacks.on_generic_access("Opening mutant", name);
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
            e.inherit = object_attributes && (object_attributes.read().Attributes & OBJ_INHERIT) != 0;

            if (initial_owner)
            {
                e.try_lock(mutant::make_owner(c.proc.process_id, c.thread().id));
            }

            const auto handle = c.proc.mutants.store(std::move(e));
            mutant_handle.write(handle);

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
