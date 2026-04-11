#include "../std_include.hpp"
#include "platform/namespace.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    namespace
    {
        bool check_sid(const SID* sid)
        {
            if (sid->Revision != SID_REVISION || sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES)
            {
                return false;
            }

            return true;
        }

        NTSTATUS open_or_create_ns(const syscall_context& c, const emulator_object<handle> namespace_handle,
                                   const ACCESS_MASK desired_access,
                                   const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                   const emulator_object<OBJECT_BOUNDARY_DESCRIPTOR> boundary_descriptor, const bool open)
        {
            if (!namespace_handle.value() || !boundary_descriptor.value())
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (object_attributes.value())
            {
                const auto attributes = object_attributes.read();
                if (attributes.RootDirectory || attributes.ObjectName)
                {
                    return STATUS_OBJECT_NAME_INVALID;
                }
            }

            const auto descriptor = boundary_descriptor.try_read();
            if (!descriptor.has_value() || descriptor->TotalSize < sizeof(OBJECT_BOUNDARY_DESCRIPTOR))
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto entry_addr = boundary_descriptor.value() + sizeof(OBJECT_BOUNDARY_DESCRIPTOR);
            const auto buffer_end = entry_addr + descriptor->TotalSize;

            if (buffer_end < entry_addr)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::u16string boundary_name{};
            int names_count = 0;
            int il_count = 0;
            int sid_count = 0;

            for (ULONG i = 0; i < descriptor->Items; i++)
            {
                const auto entry = c.emu.read_memory<OBJECT_BOUNDARY_ENTRY>(entry_addr);

                if (entry.EntrySize < sizeof(OBJECT_BOUNDARY_ENTRY) || entry_addr + entry.EntrySize > buffer_end)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (entry.EntryType == OBNS_Name)
                {
                    if (++names_count > 1)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto name_length = entry.EntrySize - sizeof(OBJECT_BOUNDARY_ENTRY);
                    boundary_name.resize(name_length / 2);
                    c.emu.read_memory(entry_addr + sizeof(OBJECT_BOUNDARY_ENTRY), boundary_name.data(), name_length);
                }
                else if (entry.EntryType == OBNS_IL)
                {
                    if (++il_count > 1)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    emulator_object<SID> sid_object(c.emu, entry_addr + sizeof(OBJECT_BOUNDARY_ENTRY));
                    const auto sid = sid_object.read();

                    if (!check_sid(&sid))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                }
                else if (entry.EntryType == OBNS_SID)
                {
                    emulator_object<SID> sid_object(c.emu, entry_addr + sizeof(OBJECT_BOUNDARY_ENTRY));
                    const auto sid = sid_object.read();

                    if (!check_sid(&sid))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    sid_count++;
                }
                else
                {
                    return STATUS_INVALID_PARAMETER;
                }

                entry_addr = align_up(entry_addr + entry.EntrySize, 8);
            }

            if (names_count == 0 || sid_count == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            for (auto& entry : c.proc.private_namespaces)
            {
                if (entry.second.boundary_name == boundary_name)
                {
                    if (entry.second.deleted)
                    {
                        return STATUS_OBJECT_PATH_NOT_FOUND;
                    }

                    if (!open)
                    {
                        return STATUS_OBJECT_NAME_COLLISION;
                    }

                    ++entry.second.ref_count;

                    c.win_emu.callbacks.on_generic_access("Opening private namespace", boundary_name);
                    namespace_handle.write(c.proc.private_namespaces.make_handle(entry.first));
                    return STATUS_SUCCESS;
                }
            }

            if (open)
            {
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }

            c.win_emu.callbacks.on_generic_access("Creating private namespace", boundary_name);

            private_namespace ns{};
            ns.boundary_name = std::move(boundary_name);
            ns.access_mask = desired_access;

            const auto handle = c.proc.private_namespaces.store(std::move(ns));
            namespace_handle.write(handle);

            return STATUS_SUCCESS;
        }
    }

    NTSTATUS handle_NtCreatePrivateNamespace(const syscall_context& c, const emulator_object<handle> namespace_handle,
                                             const ACCESS_MASK desired_access,
                                             const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                             const emulator_object<OBJECT_BOUNDARY_DESCRIPTOR> boundary_descriptor)
    {
        return open_or_create_ns(c, namespace_handle, desired_access, object_attributes, boundary_descriptor, false);
    }

    NTSTATUS handle_NtOpenPrivateNamespace(const syscall_context& c, const emulator_object<handle> namespace_handle,
                                           const ACCESS_MASK desired_access,
                                           const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                           const emulator_object<OBJECT_BOUNDARY_DESCRIPTOR> boundary_descriptor)
    {
        return open_or_create_ns(c, namespace_handle, desired_access, object_attributes, boundary_descriptor, true);
    }

    NTSTATUS handle_NtDeletePrivateNamespace(const syscall_context& c, const handle namespace_handle)
    {
        auto* ns = c.proc.private_namespaces.get(namespace_handle);
        if (ns == nullptr)
        {
            return STATUS_INVALID_HANDLE;
        }

        ns->deleted = true;
        c.proc.private_namespaces.erase(namespace_handle);

        return STATUS_SUCCESS;
    }
}
