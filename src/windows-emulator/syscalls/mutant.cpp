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
                return STATUS_OBJECT_TYPE_MISMATCH;
            }

            auto* mutant = c.proc.mutants.get(mutant_handle);
            if (!mutant)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto [old_count, succeeded] = mutant->release(c.thread().id);

            if (previous_count)
            {
                previous_count.write(static_cast<LONG>(old_count));
            }

            if (!succeeded)
            {
                return STATUS_MUTANT_NOT_OWNED;
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
                e.try_lock(c.thread().id);
            }

            const auto handle = c.proc.mutants.store(std::move(e));
            mutant_handle.write(handle);

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryMutant(const syscall_context& c, const handle mutant_handle, const ULONG mutant_information_class,
                                      const uint64_t mutant_information, const ULONG mutant_information_length,
                                      const emulator_object<ULONG> return_length)
        {
            ULONG required_length{};
            switch (mutant_information_class)
            {
            case MutantBasicInformation:
                required_length = sizeof(MUTANT_BASIC_INFORMATION);
                break;
            case MutantOwnerInformation:
                required_length = sizeof(MUTANT_OWNER_INFORMATION);
                break;
            default:
                return STATUS_INVALID_INFO_CLASS;
            }

            if (mutant_information_length != required_length)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            if (mutant_handle.value.type != handle_types::mutant)
            {
                return mutant_handle.value.type == handle_types::reserved ? STATUS_INVALID_HANDLE : STATUS_OBJECT_TYPE_MISMATCH;
            }

            const auto* mutant = c.proc.mutants.get(mutant_handle);
            if (!mutant)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto is_owned = mutant->locked_count > 0;

            if (mutant_information_class == MutantBasicInformation)
            {
                emulator_object<MUTANT_BASIC_INFORMATION>{c.emu, mutant_information}.access([&](MUTANT_BASIC_INFORMATION& info) {
                    info.CurrentCount = 1 - static_cast<LONG>(mutant->locked_count);
                    info.OwnedByCaller = is_owned && mutant->owning_thread_id == c.thread().id;
                    info.AbandonedState = mutant->abandoned;
                });
            }
            else
            {
                MUTANT_OWNER_INFORMATION info{};
                if (is_owned)
                {
                    info.ClientId.UniqueProcess = c.proc.process_id;
                    info.ClientId.UniqueThread = mutant->owning_thread_id;
                }

                emulator_object<MUTANT_OWNER_INFORMATION>{c.emu, mutant_information}.write(info);
            }

            return_length.write_if_valid(required_length);
            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
