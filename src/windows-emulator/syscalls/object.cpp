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

        auto* handle_store = c.proc.get_handle_store(h);
        if (handle_store && handle_store->erase(h))
        {
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS handle_NtDuplicateObject(const syscall_context& c, const handle source_process_handle, const handle source_handle,
                                      const handle target_process_handle, const emulator_object<handle> target_handle,
                                      const ACCESS_MASK /*desired_access*/, const ULONG /*handle_attributes*/, const ULONG /*options*/)
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

    std::u16string get_type_name(const handle_types::type type)
    {
        switch (type)
        {
        case handle_types::file:
            return u"File";
        case handle_types::device:
            return u"Device";
        case handle_types::event:
            return u"Event";
        case handle_types::section:
            return u"Section";
        case handle_types::symlink:
            return u"Symlink";
        case handle_types::directory:
            return u"Directory";
        case handle_types::semaphore:
            return u"Semaphore";
        case handle_types::port:
            return u"Port";
        case handle_types::thread:
            return u"Thread";
        case handle_types::registry:
            return u"Registry";
        case handle_types::mutant:
            return u"Mutant";
        case handle_types::token:
            return u"Token";
        case handle_types::window:
            return u"Window";
        case handle_types::timer:
            return u"Timer";
        default:
            return u"";
        }
    }

    NTSTATUS handle_NtQueryObject(const syscall_context& c, const handle handle, const OBJECT_INFORMATION_CLASS object_information_class,
                                  const emulator_pointer object_information, const ULONG object_information_length,
                                  const emulator_object<ULONG> return_length)
    {
        if (object_information_class == ObjectNameInformation)
        {
            std::u16string device_path;
            switch (handle.value.type)
            {
            case handle_types::reserved: {
                return STATUS_NOT_SUPPORTED;
            }

            case handle_types::file: {
                const auto* file = c.proc.files.get(handle);
                if (!file)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = windows_path(file->name).to_device_path();
                break;
            }
            case handle_types::device: {
                const auto* device = c.proc.devices.get(handle);
                if (!device)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = device->get_device_path();
                break;
            }
            case handle_types::directory: {
                // Directory handles are pseudo handles representing specific object directories
                if (handle == KNOWN_DLLS_DIRECTORY)
                {
                    device_path = u"\\KnownDlls";
                }
                else if (handle == KNOWN_DLLS32_DIRECTORY)
                {
                    device_path = u"\\KnownDlls32";
                }
                else if (handle == BASE_NAMED_OBJECTS_DIRECTORY)
                {
                    device_path = u"\\Sessions\\1\\BaseNamedObjects";
                }
                else if (handle == RPC_CONTROL_DIRECTORY)
                {
                    device_path = u"\\RPC Control";
                }
                else
                {
                    // Unknown directory handle
                    return STATUS_INVALID_HANDLE;
                }
                break;
            }
            case handle_types::registry: {
                const auto* registry = c.proc.registry_keys.get(handle);
                if (!registry)
                {
                    return STATUS_INVALID_HANDLE;
                }

                // Build the full registry path in device format
                auto registry_path = (registry->hive.get() / registry->path.get()).u16string();

                // Convert backslashes to forward slashes for consistency
                std::ranges::replace(registry_path, u'/', u'\\');

                // Convert to uppercase as Windows registry paths are case-insensitive
                std::ranges::transform(registry_path, registry_path.begin(), std::towupper);

                device_path = registry_path;
                break;
            }
            default:
                c.win_emu.log.error("Unsupported handle type for name information query: %X\n", handle.value.type);
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }

            const auto required_size = sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + (device_path.size() + 1) * 2;
            return_length.write_if_valid(static_cast<ULONG>(required_size));

            if (required_size > object_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_allocator allocator(c.emu, object_information, object_information_length);
            allocator.make_unicode_string(device_path);

            return STATUS_SUCCESS;
        }

        if (object_information_class == ObjectTypeInformation)
        {
            const auto name = get_type_name(static_cast<handle_types::type>(handle.value.type));

            const auto required_size = sizeof(OBJECT_TYPE_INFORMATION) + (name.size() + 1) * 2;
            return_length.write_if_valid(static_cast<ULONG>(required_size));

            if (required_size > object_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_allocator allocator(c.emu, object_information, object_information_length);
            const auto info = allocator.reserve<OBJECT_TYPE_INFORMATION>();
            info.access([&](OBJECT_TYPE_INFORMATION& i) {
                allocator.make_unicode_string(i.TypeName, name); //
            });

            return STATUS_SUCCESS;
        }

        if (object_information_class == ObjectTypesInformation)
        {
            const auto name = get_type_name(static_cast<handle_types::type>(handle.value.type));
            constexpr auto type_start_offset = align_up(sizeof(OBJECT_TYPES_INFORMATION), sizeof(uint64_t));

            const auto required_size = type_start_offset + sizeof(OBJECT_TYPE_INFORMATION) + (name.size() + 1) * 2;
            return_length.write_if_valid(static_cast<ULONG>(required_size));

            if (required_size > object_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_allocator allocator(c.emu, object_information, object_information_length);
            const auto types_info = allocator.reserve<OBJECT_TYPES_INFORMATION>();
            types_info.access([&](OBJECT_TYPES_INFORMATION& i) {
                i.NumberOfTypes = 1; //
            });

            allocator.skip_until(type_start_offset);

            const auto info = allocator.reserve<OBJECT_TYPE_INFORMATION>();
            info.access([&](OBJECT_TYPE_INFORMATION& i) {
                allocator.make_unicode_string(i.TypeName, name); //
            });

            return STATUS_SUCCESS;
        }

        if (object_information_class == ObjectHandleFlagInformation)
        {
            return handle_query<OBJECT_HANDLE_FLAG_INFORMATION>(c.emu, object_information, object_information_length, return_length,
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

    NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, const ULONG count, const emulator_object<handle> handles,
                                             const WAIT_TYPE wait_type, const BOOLEAN alertable,
                                             const emulator_object<LARGE_INTEGER> timeout)
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
                c.win_emu.log.warn("Unsupported handle type for NtWaitForMultipleObjects: %d!\n", h.value.type);
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
            c.win_emu.log.warn("Unsupported handle type for NtWaitForSingleObject: %d!\n", h.value.type);
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

    NTSTATUS handle_NtQuerySecurityObject(const syscall_context& c, const handle /*h*/, const SECURITY_INFORMATION security_information,
                                          const emulator_pointer security_descriptor, const ULONG length,
                                          const emulator_object<ULONG> length_needed)
    {
        if ((security_information &
             (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) == 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        // Owner SID: S-1-5-32-544 (Administrators)
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        const uint8_t owner_sid[] = {0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00};

        // Group SID: S-1-5-18 (Local System)
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        const uint8_t group_sid[] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00};

        // DACL structure
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        const uint8_t dacl_data[] = {
            0x02, 0x00, 0x9C, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x05, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x0F, 0x00, 0x0F, 0x00, 0x01, 0x02, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x14, 0x00, 0x00, 0x00, 0x00, 0xE0,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x14, 0x00, 0x00, 0x00, 0x00, 0xE0,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x18, 0x00, 0x00, 0x00, 0x00, 0x10,
            0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x14, 0x00,
            0x00, 0x00, 0x00, 0x10, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00};

        // SACL structure
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
        const uint8_t sacl_data[] = {0x02, 0x00, 0x1C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x11, 0x00, 0x14, 0x00, 0x01, 0x00,
                                     0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00};

        ULONG total_size = sizeof(SECURITY_DESCRIPTOR_RELATIVE);

        if (security_information & OWNER_SECURITY_INFORMATION)
        {
            total_size += sizeof(owner_sid);
        }

        if (security_information & GROUP_SECURITY_INFORMATION)
        {
            total_size += sizeof(group_sid);
        }

        if (security_information & DACL_SECURITY_INFORMATION)
        {
            total_size += sizeof(dacl_data);
        }

        if (security_information & LABEL_SECURITY_INFORMATION)
        {
            total_size += sizeof(sacl_data);
        }

        length_needed.write(total_size);

        if (length < total_size)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        if (!security_descriptor)
        {
            return STATUS_INVALID_PARAMETER;
        }

        SECURITY_DESCRIPTOR_RELATIVE sd = {};
        sd.Revision = SECURITY_DESCRIPTOR_REVISION;
        sd.Control = SE_SELF_RELATIVE;

        ULONG current_offset = sizeof(sd);

        if (security_information & OWNER_SECURITY_INFORMATION)
        {
            sd.Owner = current_offset;
            c.emu.write_memory(security_descriptor + current_offset, owner_sid);
            current_offset += sizeof(owner_sid);
        }

        if (security_information & GROUP_SECURITY_INFORMATION)
        {
            sd.Group = current_offset;
            c.emu.write_memory(security_descriptor + current_offset, group_sid);
            current_offset += sizeof(group_sid);
        }

        if (security_information & DACL_SECURITY_INFORMATION)
        {
            sd.Control |= SE_DACL_PRESENT;
            sd.Dacl = current_offset;
            c.emu.write_memory(security_descriptor + current_offset, dacl_data);
            current_offset += sizeof(dacl_data);
        }

        if (security_information & LABEL_SECURITY_INFORMATION)
        {
            sd.Control |= SE_SACL_PRESENT | SE_SACL_AUTO_INHERITED;
            sd.Sacl = current_offset;
            c.emu.write_memory(security_descriptor + current_offset, sacl_data);
            current_offset += sizeof(sacl_data);
        }

        assert(current_offset == total_size);

        c.emu.write_memory(security_descriptor, sd);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetSecurityObject()
    {
        return STATUS_SUCCESS;
    }
}
