#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../io_completion_wait.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtClose(const syscall_context& c, const handle h)
    {
        const auto value = h.value;

        if (h.h == 0xDEADC0DE || h.h == 0xDEADBEEF)
        {
            c.win_emu.callbacks.on_suspicious_activity("Anti-debug check with invalid handle");

            return STATUS_INVALID_HANDLE;
        }

        if (value.is_pseudo)
        {
            return STATUS_SUCCESS;
        }

        if (value.type == handle_types::wait_completion_packet)
        {
            auto* wait_packet = c.proc.wait_completion_packets.get(h);
            if (wait_packet && wait_packet->ref_count == 1)
            {
                io_completion_wait::cleanup_wait_packet_on_close(c.proc, h);
            }
        }

        if (value.type == handle_types::worker_factory)
        {
            auto* factory = c.proc.worker_factories.get(h);
            if (factory && factory->ref_count == 1)
            {
                io_completion_wait::release_handle_reference(c.proc, factory->io_completion_handle);
            }
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
        case handle_types::desktop:
            return u"Desktop";
        case handle_types::io_completion:
            return u"IoCompletion";
        case handle_types::wait_completion_packet:
            return u"WaitCompletionPacket";
        case handle_types::worker_factory:
            return u"TpWorkerFactory";
        case handle_types::private_namespace:
            return u"Directory";
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
            case handle_types::desktop: {
                const auto* desk = c.proc.desktops.get(handle);
                if (!desk)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = u"\\Windows\\Desktop\\";
                device_path.append(desk->name);
                break;
            }
            case handle_types::io_completion: {
                const auto* io = c.proc.io_completions.get(handle);
                if (!io)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = io->name;
                break;
            }
            case handle_types::wait_completion_packet: {
                const auto* packet = c.proc.wait_completion_packets.get(handle);
                if (!packet)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = packet->name;
                break;
            }
            case handle_types::worker_factory: {
                const auto* factory = c.proc.worker_factories.get(handle);
                if (!factory)
                {
                    return STATUS_INVALID_HANDLE;
                }

                device_path = factory->name;
                break;
            }
            case handle_types::private_namespace: {
                const auto* ns = c.proc.private_namespaces.get(handle);
                if (!ns)
                {
                    return STATUS_INVALID_HANDLE;
                }

                break;
            }
            default:
                c.win_emu.log.error("Unsupported handle type for name information query: %X\n", handle.value.type);
                c.emu.stop();
                return STATUS_NOT_SUPPORTED;
            }

            const auto required_size =
                sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + ((device_path.size() + (device_path.empty() ? 0 : 1)) * 2);
            return_length.write_if_valid(static_cast<ULONG>(required_size));

            if (required_size > object_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            if (device_path.empty())
            {
                UNICODE_STRING<EmulatorTraits<Emu64>> zero_buf{};
                c.emu.write_memory(object_information, zero_buf);
            }
            else
            {
                emulator_allocator allocator(c.emu, object_information, object_information_length);
                allocator.make_unicode_string(device_path);
            }

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

    template <typename Store>
    void collect_wait32_candidate(Store& store, const uint32_t id, std::optional<handle>& resolved, uint32_t& candidate_count)
    {
        if (!store.get_by_index(id))
        {
            return;
        }

        ++candidate_count;
        if (!resolved)
        {
            resolved = store.make_handle(id);
        }
    }

    std::optional<handle> resolve_wait32_handle(const syscall_context& c, const uint32_t raw_handle)
    {
        const auto decoded = make_handle(static_cast<uint64_t>(raw_handle));
        if (decoded.value.type != handle_types::reserved)
        {
            return decoded;
        }

        // wait32 can give raw 32 bit handles without type bits
        const auto id = static_cast<uint32_t>(decoded.value.id);
        if (id == 0)
        {
            return std::nullopt;
        }

        std::optional<handle> resolved{};
        uint32_t candidate_count = 0;

        collect_wait32_candidate(c.proc.events, id, resolved, candidate_count);
        collect_wait32_candidate(c.proc.threads, id, resolved, candidate_count);
        collect_wait32_candidate(c.proc.mutants, id, resolved, candidate_count);
        collect_wait32_candidate(c.proc.semaphores, id, resolved, candidate_count);
        collect_wait32_candidate(c.proc.timers, id, resolved, candidate_count);

        if (candidate_count == 1)
        {
            return resolved;
        }

        return std::nullopt;
    }

    NTSTATUS validate_wait_handle(const syscall_context& c, const handle h)
    {
        const auto validate_handle_in_store = [&](auto& store) -> NTSTATUS {
            return store.get(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        };

        switch (h.value.type)
        {
        case handle_types::event:
            if (h.value.is_pseudo)
            {
                return STATUS_SUCCESS;
            }

            return validate_handle_in_store(c.proc.events);

        case handle_types::thread:
            return validate_handle_in_store(c.proc.threads);

        case handle_types::mutant:
            return validate_handle_in_store(c.proc.mutants);

        case handle_types::semaphore:
            return validate_handle_in_store(c.proc.semaphores);

        case handle_types::timer:
            if (h.value.is_pseudo)
            {
                return STATUS_SUCCESS;
            }

            return validate_handle_in_store(c.proc.timers);

        default:
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
    }

    NTSTATUS handle_NtCompareObjects(const syscall_context&, const handle first, const handle second)
    {
        return (first == second) ? STATUS_SUCCESS : STATUS_NOT_SAME_OBJECT;
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
        t.await_objects = {};
        t.await_any = false;

        std::vector<handle> wait_handles{};
        wait_handles.reserve(count);

        for (ULONG i = 0; i < count; ++i)
        {
            const auto h = handles.read(i);

            const auto validation_status = validate_wait_handle(c, h);
            if (!NT_SUCCESS(validation_status))
            {
                t.await_time = {};
                return validation_status;
            }

            wait_handles.push_back(h);
        }

        t.await_objects = std::move(wait_handles);
        t.await_any = wait_type == WaitAny;

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());
        }

        c.win_emu.yield_thread(alertable);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtWaitForMultipleObjects32(const syscall_context& c, const ULONG count, const emulator_object<uint32_t> handles,
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
        t.await_objects = {};
        t.await_any = false;

        std::vector<handle> wait_handles{};
        wait_handles.reserve(count);

        for (ULONG i = 0; i < count; ++i)
        {
            const auto raw_handle = handles.read(i);
            const auto h = resolve_wait32_handle(c, raw_handle);
            if (!h)
            {
                t.await_time = {};
                return STATUS_INVALID_HANDLE;
            }

            const auto validation_status = validate_wait_handle(c, *h);
            if (!NT_SUCCESS(validation_status))
            {
                t.await_time = {};
                return validation_status;
            }

            wait_handles.push_back(*h);
        }

        t.await_objects = std::move(wait_handles);
        t.await_any = wait_type == WaitAny;

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
        const auto validation_status = validate_wait_handle(c, h);
        if (!NT_SUCCESS(validation_status))
        {
            return validation_status;
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
