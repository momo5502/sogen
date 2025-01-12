#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "context_frame.hpp"
#include "emulator_utils.hpp"
#include "syscall_utils.hpp"

#include <numeric>
#include <ranges>
#include <cwctype>
#include <algorithm>
#include <utils/io.hpp>
#include <utils/string.hpp>
#include <utils/time.hpp>
#include <utils/finally.hpp>

#include <sys/stat.h>

namespace
{
    NTSTATUS handle_NtQueryPerformanceCounter(const syscall_context& c,
                                              const emulator_object<LARGE_INTEGER> performance_counter,
                                              const emulator_object<LARGE_INTEGER> performance_frequency)
    {
        try
        {
            if (performance_counter)
            {
                performance_counter.access([&](LARGE_INTEGER& value) {
                    if (c.win_emu.time_is_relative())
                    {
                        value.QuadPart = static_cast<LONGLONG>(c.proc.executed_instructions);
                    }
                    else
                    {
                        value.QuadPart = std::chrono::steady_clock::now().time_since_epoch().count();
                    }
                });
            }

            if (performance_frequency)
            {
                performance_frequency.access(
                    [&](LARGE_INTEGER& value) { value.QuadPart = c.proc.kusd.get().QpcFrequency; });
            }

            return STATUS_SUCCESS;
        }
        catch (...)
        {
            return STATUS_ACCESS_VIOLATION;
        }
    }

    NTSTATUS handle_NtManageHotPatch()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtCreateWorkerFactory()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenKey(const syscall_context& c, const emulator_object<handle> key_handle,
                              const ACCESS_MASK /*desired_access*/,
                              const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        auto key =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));

        if (attributes.RootDirectory)
        {
            const auto* parent_handle = c.proc.registry_keys.get(attributes.RootDirectory);
            if (!parent_handle)
            {
                return STATUS_INVALID_HANDLE;
            }

            const std::filesystem::path full_path = parent_handle->hive / parent_handle->path / key;
            key = full_path.u16string();
        }

        c.win_emu.log.print(color::dark_gray, "--> Registry key: %s\n", u16_to_u8(key).c_str());

        auto entry = c.proc.registry.get_key(key);
        if (!entry.has_value())
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        const auto handle = c.proc.registry_keys.store(std::move(entry.value()));
        key_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenKeyEx(const syscall_context& c, const emulator_object<handle> key_handle,
                                const ACCESS_MASK desired_access,
                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                ULONG /*open_options*/)
    {
        return handle_NtOpenKey(c, key_handle, desired_access, object_attributes);
    }

    NTSTATUS handle_NtQueryKey(const syscall_context& c, const handle key_handle,
                               const KEY_INFORMATION_CLASS key_information_class, const uint64_t key_information,
                               const ULONG length, const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (key_information_class == KeyNameInformation)
        {
            auto key_name = (key->hive / key->path).wstring();
            while (key_name.ends_with('/') || key_name.ends_with('\\'))
            {
                key_name.pop_back();
            }

            std::ranges::transform(key_name, key_name.begin(), std::towupper);

            const auto required_size = sizeof(KEY_NAME_INFORMATION) + (key_name.size() * 2) - 1;
            result_length.write(static_cast<ULONG>(required_size));

            if (required_size > length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            KEY_NAME_INFORMATION info{};
            info.NameLength = static_cast<ULONG>(key_name.size() * 2);

            const emulator_object<KEY_NAME_INFORMATION> info_obj{c.emu, key_information};
            info_obj.write(info);

            c.emu.write_memory(key_information + offsetof(KEY_NAME_INFORMATION, Name), key_name.data(),
                               info.NameLength);

            return STATUS_SUCCESS;
        }

        if (key_information_class == KeyFullInformation)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (key_information_class == KeyHandleTagsInformation)
        {
            constexpr auto required_size = sizeof(KEY_HANDLE_TAGS_INFORMATION);
            result_length.write(required_size);

            if (required_size > length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            KEY_HANDLE_TAGS_INFORMATION info{};
            info.HandleTags = 0; // ?

            const emulator_object<KEY_HANDLE_TAGS_INFORMATION> info_obj{c.emu, key_information};
            info_obj.write(info);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.print(color::gray, "Unsupported registry class: %X\n", key_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryValueKey(const syscall_context& c, const handle key_handle,
                                    const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> value_name,
                                    const KEY_VALUE_INFORMATION_CLASS key_value_information_class,
                                    const uint64_t key_value_information, const ULONG length,
                                    const emulator_object<ULONG> result_length)
    {
        const auto* key = c.proc.registry_keys.get(key_handle);
        if (!key)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto query_name = read_unicode_string(c.emu, value_name);

        const auto value = c.proc.registry.get_value(*key, u16_to_u8(query_name));
        if (!value)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        const std::wstring original_name(value->name.begin(), value->name.end());

        if (key_value_information_class == KeyValueBasicInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_BASIC_INFORMATION, Name);
            const auto required_size = base_size + (original_name.size() * 2) - 1;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_BASIC_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.NameLength = static_cast<ULONG>(original_name.size() * 2);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, original_name.data(), info.NameLength);

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValuePartialInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data);
            const auto required_size = base_size + value->data.size();
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_PARTIAL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataLength = static_cast<ULONG>(value->data.size());

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, value->data.data(), value->data.size());

            return STATUS_SUCCESS;
        }

        if (key_value_information_class == KeyValueFullInformation)
        {
            constexpr auto base_size = offsetof(KEY_VALUE_FULL_INFORMATION, Name);
            const auto name_size = original_name.size() * 2;
            const auto value_size = value->data.size();
            const auto required_size = base_size + name_size + value_size + -1;
            result_length.write(static_cast<ULONG>(required_size));

            KEY_VALUE_FULL_INFORMATION info{};
            info.TitleIndex = 0;
            info.Type = value->type;
            info.DataLength = static_cast<ULONG>(value->data.size());
            info.NameLength = static_cast<ULONG>(original_name.size() * 2);

            if (base_size <= length)
            {
                c.emu.write_memory(key_value_information, &info, base_size);
            }

            if (required_size > length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(key_value_information + base_size, original_name.data(), info.NameLength);

            c.emu.write_memory(key_value_information + base_size + info.NameLength, value->data.data(),
                               value->data.size());

            return STATUS_SUCCESS;
        }

        c.win_emu.log.print(color::gray, "Unsupported registry value class: %X\n", key_value_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtCreateKey()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtNotifyChangeKey()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, const handle thread_handle,
                                           const THREADINFOCLASS info_class, const uint64_t thread_information,
                                           const uint32_t thread_information_length)
    {
        auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == ThreadSchedulerSharedDataSlot)
        {
            return STATUS_SUCCESS;
        }

        if (info_class == ThreadHideFromDebugger)
        {
            c.win_emu.log.print(color::pink, "--> Hiding thread %X from debugger!\n", thread->id);
            return STATUS_SUCCESS;
        }

        if (info_class == ThreadNameInformation)
        {
            if (thread_information_length != sizeof(THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>> info{c.emu, thread_information};
            const auto i = info.read();
            thread->name = read_unicode_string(c.emu, i.ThreadName);

            c.win_emu.log.print(color::blue, "Setting thread (%d) name: %s\n", thread->id,
                                u16_to_u8(thread->name).c_str());

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadImpersonationToken)
        {
            if (thread_information_length != sizeof(handle))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<handle> info{c.emu, thread_information};
            info.write(DUMMY_IMPERSONATION_TOKEN);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadZeroTlsCell)
        {
            if (thread_information_length != sizeof(ULONG))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto tls_index = c.emu.read_memory<ULONG>(thread_information);
            const auto teb = thread->teb->read();

            auto* tls_vector = teb.ThreadLocalStoragePointer;
            c.emu.write_memory<void*>(tls_vector + tls_index, nullptr);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported thread set info class: %X\n", info_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSetEvent(const syscall_context& c, const uint64_t handle,
                               const emulator_object<LONG> previous_state)
    {
        const auto entry = c.proc.events.get(handle);
        if (!entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (previous_state.value())
        {
            previous_state.write(entry->signaled ? 1ULL : 0ULL);
        }

        entry->signaled = true;
        return STATUS_SUCCESS;
    }

    generic_handle_store* get_handle_store(process_context& proc, const handle h)
    {
        switch (h.value.type)
        {
        case handle_types::thread:
            return &proc.threads;
        case handle_types::event:
            return &proc.events;
        case handle_types::file:
            return &proc.files;
        case handle_types::device:
            return &proc.devices;
        case handle_types::semaphore:
            return &proc.semaphores;
        case handle_types::registry:
            return &proc.registry_keys;
        case handle_types::mutant:
            return &proc.mutants;
        case handle_types::port:
            return &proc.ports;
        case handle_types::section:
            return &proc.sections;
        default:
            return nullptr;
        }
    }

    NTSTATUS handle_NtClose(const syscall_context& c, const handle h)
    {
        const auto value = h.value;
        if (value.is_pseudo)
        {
            return STATUS_SUCCESS;
        }

        auto* handle_store = get_handle_store(c.proc, h);
        if (handle_store && handle_store->erase(h))
        {
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS handle_NtTraceEvent()
    {
        return STATUS_SUCCESS;
    }

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

        const auto old_count = mutant->release();

        if (previous_count)
        {
            previous_count.write(static_cast<LONG>(old_count));
        }

        return STATUS_SUCCESS;
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
                name = read_unicode_string(
                    c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
            }
        }

        if (!name.empty())
        {
            for (const auto& mutant : c.proc.mutants | std::views::values)
            {
                if (mutant.name == name)
                {
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

    NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<handle> event_handle,
                                  const ACCESS_MASK /*desired_access*/,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                  const EVENT_TYPE event_type, const BOOLEAN initial_state)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                name = read_unicode_string(
                    c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));
            }
        }

        if (!name.empty())
        {
            for (const auto& event : c.proc.events | std::views::values)
            {
                if (event.name == name)
                {
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        event e{};
        e.type = event_type;
        e.signaled = initial_state != FALSE;
        e.name = std::move(name);

        const auto handle = c.proc.events.store(std::move(e));
        event_handle.write(handle);

        static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));
        static_assert(sizeof(ACCESS_MASK) == sizeof(uint32_t));

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
                                const ACCESS_MASK /*desired_access*/,
                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto name =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));

        for (auto& entry : c.proc.events)
        {
            if (entry.second.name == name)
            {
                ++entry.second.ref_count;
                event_handle.write(c.proc.events.make_handle(entry.first).bits);
                return STATUS_SUCCESS;
            }
        }

        return STATUS_NOT_FOUND;
    }

    NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, const handle file_handle,
                                                 const uint64_t /*io_status_block*/, const uint64_t fs_information,
                                                 const ULONG /*length*/,
                                                 const FS_INFORMATION_CLASS fs_information_class)
    {
        if (fs_information_class != FileFsDeviceInformation)
        {
            c.win_emu.log.error("Unsupported fs info class: %X\n", fs_information_class);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        const emulator_object<FILE_FS_DEVICE_INFORMATION> info_obj{c.emu, fs_information};
        info_obj.access([&](FILE_FS_DEVICE_INFORMATION& info) {
            if (file_handle == STDOUT_HANDLE.bits && !c.win_emu.buffer_stdout)
            {
                info.DeviceType = FILE_DEVICE_CONSOLE;
                info.Characteristics = 0x20000;
            }
            else
            {
                info.DeviceType = FILE_DEVICE_DISK;
                info.Characteristics = 0x20020;
            }
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                  const ACCESS_MASK /*desired_access*/,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();

        auto filename =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));
        c.win_emu.log.print(color::dark_gray, "--> Opening section: %s\n", u16_to_u8(filename).c_str());

        if (filename == u"\\Windows\\SharedSection")
        {
            section_handle.write(SHARED_SECTION);
            return STATUS_SUCCESS;
        }

        if (attributes.RootDirectory != KNOWN_DLLS_DIRECTORY)
        {
            puts("Unsupported section");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        utils::string::to_lower_inplace(filename);

        for (auto& section_entry : c.proc.sections)
        {
            if (section_entry.second.is_image() && section_entry.second.name == filename)
            {
                section_handle.write(c.proc.sections.make_handle(section_entry.first));
                return STATUS_SUCCESS;
            }
        }

        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS handle_NtMapViewOfSection(
        const syscall_context& c, const handle section_handle, const handle process_handle,
        const emulator_object<uint64_t> base_address,
        const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) /*zero_bits*/,
        const EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) /*commit_size*/,
        const emulator_object<LARGE_INTEGER> /*section_offset*/,
        const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
        const SECTION_INHERIT /*inherit_disposition*/, const ULONG /*allocation_type*/, const ULONG /*win32_protect*/)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_handle == SHARED_SECTION)
        {
            constexpr auto shared_section_size = 0x10000;

            const auto address = c.emu.find_free_allocation_base(shared_section_size);
            c.emu.allocate_memory(address, shared_section_size, memory_permission::read_write);

            const std::u16string_view windows_dir = c.proc.kusd.get().NtSystemRoot.arr;
            const auto windows_dir_size = windows_dir.size() * 2;

            constexpr auto windows_dir_offset = 0x10;
            c.emu.write_memory(address + 8, windows_dir_offset);

            const auto obj_address = address + windows_dir_offset;

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> windir_obj{c.emu, obj_address};
            windir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                const auto dir_address = kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, NtSystemRoot);

                ucs.Buffer = dir_address - obj_address;
                ucs.Length = static_cast<uint16_t>(windows_dir_size);
                ucs.MaximumLength = ucs.Length;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> sysdir_obj{c.emu, windir_obj.value() +
                                                                                               windir_obj.size()};
            sysdir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"C:\\WINDOWS\\System32");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> base_dir_obj{c.emu, sysdir_obj.value() +
                                                                                                 sysdir_obj.size()};
            base_dir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"\\Sessions\\1\\BaseNamedObjects");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            if (view_size)
            {
                view_size.write(shared_section_size);
            }

            base_address.write(address);

            return STATUS_SUCCESS;
        }

        const auto section_entry = c.proc.sections.get(section_handle);
        if (!section_entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_entry->is_image())
        {
            const auto binary = c.proc.mod_manager.map_module(section_entry->file_name, c.win_emu.log);
            if (!binary)
            {
                return STATUS_FILE_INVALID;
            }

            std::u16string wide_name(binary->name.begin(), binary->name.end());
            section_entry->name = utils::string::to_lower_consume(wide_name);

            if (view_size.value())
            {
                view_size.write(binary->size_of_image);
            }

            base_address.write(binary->image_base);

            return STATUS_SUCCESS;
        }

        uint64_t size = section_entry->maximum_size;
        std::vector<uint8_t> file_data{};

        if (!section_entry->file_name.empty())
        {
            if (!utils::io::read_file(section_entry->file_name, &file_data))
            {
                return STATUS_INVALID_PARAMETER;
            }

            size = page_align_up(file_data.size());
        }

        const auto protection = map_nt_to_emulator_protection(section_entry->section_page_protection);
        const auto address = c.emu.allocate_memory(size, protection);

        if (!file_data.empty())
        {
            c.emu.write_memory(address, file_data.data(), file_data.size());
        }

        if (view_size)
        {
            view_size.write(size);
        }

        base_address.write(address);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateIoCompletion(
        const syscall_context& c, const emulator_object<handle> event_handle, const ACCESS_MASK desired_access,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
        const uint32_t /*number_of_concurrent_threads*/)
    {
        return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
    }

    NTSTATUS handle_NtCreateWaitCompletionPacket(
        const syscall_context& c, const emulator_object<handle> event_handle, const ACCESS_MASK desired_access,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        return handle_NtCreateEvent(c, event_handle, desired_access, object_attributes, NotificationEvent, FALSE);
    }

    NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, const handle process_handle,
                                         const uint64_t base_address, const uint32_t info_class,
                                         const uint64_t memory_information, const uint32_t memory_information_length,
                                         const emulator_object<uint32_t> return_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == MemoryWorkingSetExInformation || info_class == MemoryImageExtensionInformation)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == MemoryBasicInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(EMU_MEMORY_BASIC_INFORMATION64));
            }

            if (memory_information_length != sizeof(EMU_MEMORY_BASIC_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<EMU_MEMORY_BASIC_INFORMATION64> info{c.emu, memory_information};

            info.access([&](EMU_MEMORY_BASIC_INFORMATION64& image_info) {
                const auto region_info = c.emu.get_region_info(base_address);

                assert(!region_info.is_committed || region_info.is_reserved);

                image_info.BaseAddress = reinterpret_cast<void*>(region_info.start);
                image_info.AllocationBase = reinterpret_cast<void*>(region_info.allocation_base);
                image_info.AllocationProtect = 0;
                image_info.PartitionId = 0;
                image_info.RegionSize = static_cast<int64_t>(region_info.length);
                image_info.State =
                    region_info.is_committed ? MEM_COMMIT : (region_info.is_reserved ? MEM_RESERVE : MEM_FREE);
                image_info.Protect = map_emulator_to_nt_protection(region_info.permissions);
                image_info.Type = MEM_PRIVATE;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == MemoryImageInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(MEMORY_IMAGE_INFORMATION64));
            }

            if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto mod = c.proc.mod_manager.find_by_address(base_address);
            if (!mod)
            {
                c.win_emu.log.error("Bad address for memory image request: 0x%" PRIx64 "\n", base_address);
                return STATUS_INVALID_ADDRESS;
            }

            const emulator_object<MEMORY_IMAGE_INFORMATION64> info{c.emu, memory_information};

            info.access([&](MEMORY_IMAGE_INFORMATION64& image_info) {
                image_info.ImageBase = reinterpret_cast<void*>(mod->image_base);
                image_info.SizeOfImage = mod->size_of_image;
                image_info.ImageFlags = 0;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == MemoryRegionInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(MEMORY_REGION_INFORMATION64));
            }

            if (memory_information_length != sizeof(MEMORY_REGION_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto region_info = c.emu.get_region_info(base_address);
            if (!region_info.is_reserved)
            {
                return STATUS_INVALID_ADDRESS;
            }

            const emulator_object<MEMORY_REGION_INFORMATION64> info{c.emu, memory_information};

            info.access([&](MEMORY_REGION_INFORMATION64& image_info) {
                memset(&image_info, 0, sizeof(image_info));

                image_info.AllocationBase = reinterpret_cast<void*>(region_info.allocation_base);
                image_info.AllocationProtect = 0;
                image_info.PartitionId = 0;
                image_info.RegionSize = static_cast<int64_t>(region_info.allocation_length);
                image_info.Reserved = 0x10;
            });

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported memory info class: %X\n", info_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQuerySystemInformation(const syscall_context& c, const uint32_t info_class,
                                             const uint64_t system_information,
                                             const uint32_t system_information_length,
                                             const emulator_object<uint32_t> return_length)
    {
        if (info_class == SystemFlushInformation || info_class == SystemHypervisorSharedPageInformation ||
            info_class == 250 // Build 27744
        )
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == SystemTimeOfDayInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(SYSTEM_TIMEOFDAY_INFORMATION64));
            }

            if (system_information_length != sizeof(SYSTEM_TIMEOFDAY_INFORMATION64))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<SYSTEM_TIMEOFDAY_INFORMATION64> info_obj{c.emu, system_information};

            info_obj.access([&](SYSTEM_TIMEOFDAY_INFORMATION64& info) {
                info.BootTime.QuadPart = 0;
                // TODO: Fill
            });

            return STATUS_SUCCESS;
        }

        if (info_class == SystemRangeStartInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(SYSTEM_RANGE_START_INFORMATION64));
            }

            if (system_information_length != sizeof(SYSTEM_RANGE_START_INFORMATION64))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<SYSTEM_RANGE_START_INFORMATION64> info_obj{c.emu, system_information};

            info_obj.access(
                [&](SYSTEM_RANGE_START_INFORMATION64& info) { info.SystemRangeStart = 0xFFFF800000000000; });

            return STATUS_SUCCESS;
        }

        if (info_class == SystemProcessorInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(SYSTEM_PROCESSOR_INFORMATION64));
            }

            if (system_information_length != sizeof(SYSTEM_PROCESSOR_INFORMATION64))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<SYSTEM_PROCESSOR_INFORMATION64> info_obj{c.emu, system_information};

            info_obj.access([&](SYSTEM_PROCESSOR_INFORMATION64& info) {
                memset(&info, 0, sizeof(info));
                info.MaximumProcessors = 2;
                info.ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == SystemNumaProcessorMap)
        {
            if (return_length)
            {
                return_length.write(sizeof(SYSTEM_NUMA_INFORMATION64));
            }

            if (system_information_length != sizeof(SYSTEM_NUMA_INFORMATION64))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<SYSTEM_NUMA_INFORMATION64> info_obj{c.emu, system_information};

            info_obj.access([&](SYSTEM_NUMA_INFORMATION64& info) {
                memset(&info, 0, sizeof(info));
                info.ActiveProcessorsGroupAffinity->Mask = 0xFFF;
                info.AvailableMemory[0] = 0xFFF;
                info.Pad[0] = 0xFFF;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == SystemErrorPortTimeouts)
        {
            if (return_length)
            {
                return_length.write(sizeof(SYSTEM_ERROR_PORT_TIMEOUTS));
            }

            if (system_information_length != sizeof(SYSTEM_ERROR_PORT_TIMEOUTS))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<SYSTEM_ERROR_PORT_TIMEOUTS> info_obj{c.emu, system_information};

            info_obj.access([&](SYSTEM_ERROR_PORT_TIMEOUTS& info) {
                info.StartTimeout = 0;
                info.CommTimeout = 0;
            });

            return STATUS_SUCCESS;
        }

        if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
        {
            c.win_emu.log.error("Unsupported system info class: %X\n", info_class);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        if (return_length)
        {
            return_length.write(sizeof(SYSTEM_BASIC_INFORMATION64));
        }

        if (system_information_length != sizeof(SYSTEM_BASIC_INFORMATION64))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        const emulator_object<SYSTEM_BASIC_INFORMATION64> info{c.emu, system_information};

        info.access([&](SYSTEM_BASIC_INFORMATION64& basic_info) {
            basic_info.Reserved = 0;
            basic_info.TimerResolution = 0x0002625a;
            basic_info.PageSize = 0x1000;
            basic_info.LowestPhysicalPageNumber = 0x00000001;
            basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
            basic_info.AllocationGranularity = 0x10000;
            basic_info.MinimumUserModeAddress = 0x0000000000010000;
            basic_info.MaximumUserModeAddress = 0x00007ffffffeffff;
            basic_info.ActiveProcessorsAffinityMask = 0x0000000000000fff;
            basic_info.NumberOfProcessors = 1;
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtDuplicateObject(const syscall_context& /*c*/, const handle source_process_handle,
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

        puts("Duplicating non-pseudo object not supported yet!");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQuerySystemInformationEx(const syscall_context& c, const uint32_t info_class,
                                               const uint64_t input_buffer, const uint32_t input_buffer_length,
                                               const uint64_t system_information,
                                               const uint32_t system_information_length,
                                               const emulator_object<uint32_t> return_length)
    {
        if (info_class == SystemFlushInformation || info_class == SystemFeatureConfigurationInformation ||
            info_class == SystemSupportedProcessorArchitectures2 ||
            info_class == SystemFeatureConfigurationSectionInformation)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == SystemLogicalProcessorAndGroupInformation)
        {
            if (input_buffer_length != sizeof(LOGICAL_PROCESSOR_RELATIONSHIP))
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto request = c.emu.read_memory<LOGICAL_PROCESSOR_RELATIONSHIP>(input_buffer);

            if (request == RelationGroup)
            {
                constexpr auto root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, Group);
                constexpr auto required_size = root_size + sizeof(EMU_GROUP_RELATIONSHIP64);

                if (return_length)
                {
                    return_length.write(required_size);
                }

                if (system_information_length < required_size)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                proc_info.Size = required_size;
                proc_info.Relationship = RelationGroup;

                c.emu.write_memory(system_information, &proc_info, root_size);

                EMU_GROUP_RELATIONSHIP64 group{};
                group.ActiveGroupCount = 1;
                group.MaximumGroupCount = 1;

                auto& group_info = group.GroupInfo[0];
                group_info.ActiveProcessorCount = static_cast<uint8_t>(c.proc.kusd.get().ActiveProcessorCount);
                group_info.ActiveProcessorMask = (1 << group_info.ActiveProcessorCount) - 1;
                group_info.MaximumProcessorCount = group_info.ActiveProcessorCount;

                c.emu.write_memory(system_information + root_size, group);
                return STATUS_SUCCESS;
            }

            if (request == RelationNumaNode || request == RelationNumaNodeEx)
            {
                constexpr auto root_size = offsetof(EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64, NumaNode);
                constexpr auto required_size = root_size + sizeof(EMU_NUMA_NODE_RELATIONSHIP64);

                if (return_length)
                {
                    return_length.write(required_size);
                }

                if (system_information_length < required_size)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                EMU_SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX64 proc_info{};
                proc_info.Size = required_size;
                proc_info.Relationship = RelationNumaNode;

                c.emu.write_memory(system_information, &proc_info, root_size);

                EMU_NUMA_NODE_RELATIONSHIP64 numa_node{};
                memset(&numa_node, 0, sizeof(numa_node));

                c.emu.write_memory(system_information + root_size, numa_node);
                return STATUS_SUCCESS;
            }

            c.win_emu.log.error("Unsupported processor relationship: %X\n", request);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class != SystemBasicInformation && info_class != SystemEmulationBasicInformation)
        {
            c.win_emu.log.error("Unsupported system info ex class: %X\n", info_class);
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        if (return_length)
        {
            return_length.write(sizeof(SYSTEM_BASIC_INFORMATION64));
        }

        if (system_information_length < sizeof(SYSTEM_BASIC_INFORMATION64))
        {
            return STATUS_INFO_LENGTH_MISMATCH;
        }

        const emulator_object<SYSTEM_BASIC_INFORMATION64> info{c.emu, system_information};

        info.access([&](SYSTEM_BASIC_INFORMATION64& basic_info) {
            basic_info.Reserved = 0;
            basic_info.TimerResolution = 0x0002625a;
            basic_info.PageSize = 0x1000;
            basic_info.LowestPhysicalPageNumber = 0x00000001;
            basic_info.HighestPhysicalPageNumber = 0x00c9c7ff;
            basic_info.AllocationGranularity = 0x10000;
            basic_info.MinimumUserModeAddress = 0x0000000000010000;
            basic_info.MaximumUserModeAddress = 0x00007ffffffeffff;
            basic_info.ActiveProcessorsAffinityMask = 0x0000000000000fff;
            basic_info.NumberOfProcessors = 1;
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, const handle process_handle,
                                              const uint32_t info_class, const uint64_t process_information,
                                              const uint32_t process_information_length,
                                              const emulator_object<uint32_t> return_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == ProcessImageInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>));
            }

            if (process_information_length != sizeof(SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>> info{c.emu, process_information};
            info.access([&](SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>& i) {
                const auto& mod = *c.proc.executable;

                const emulator_object<PEDosHeader_t> dos_header_obj{c.emu, mod.image_base};
                const auto dos_header = dos_header_obj.read();

                const emulator_object<PENTHeaders_t<uint64_t>> nt_headers_obj{c.emu,
                                                                              mod.image_base + dos_header.e_lfanew};
                const auto nt_headers = nt_headers_obj.read();

                const auto& file_header = nt_headers.FileHeader;
                const auto& optional_header = nt_headers.OptionalHeader;

                i.TransferAddress = 0;
                i.MaximumStackSize = optional_header.SizeOfStackReserve;
                i.CommittedStackSize = optional_header.SizeOfStackCommit;
                i.SubSystemType = optional_header.Subsystem;
                i.SubSystemMajorVersion = optional_header.MajorSubsystemVersion;
                i.SubSystemMinorVersion = optional_header.MinorSubsystemVersion;
                i.MajorOperatingSystemVersion = optional_header.MajorOperatingSystemVersion;
                i.MinorOperatingSystemVersion = optional_header.MinorOperatingSystemVersion;
                i.ImageCharacteristics = file_header.Characteristics;
                i.DllCharacteristics = optional_header.DllCharacteristics;
                i.Machine = file_header.Machine;
                i.ImageContainsCode = TRUE;
                i.ImageFlags = 0; // TODO
                i.ImageFileSize = optional_header.SizeOfImage;
                i.LoaderFlags = optional_header.LoaderFlags;
                i.CheckSum = optional_header.CheckSum;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessCookie)
        {
            if (return_length)
            {
                return_length.write(sizeof(uint32_t));
            }

            if (process_information_length != sizeof(uint32_t))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<uint32_t> info{c.emu, process_information};
            info.write(0x01234567);

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessDebugPort)
        {
            if (return_length)
            {
                return_length.write(sizeof(EmulatorTraits<Emu64>::PVOID));
            }

            if (process_information_length != sizeof(EmulatorTraits<Emu64>::PVOID))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<EmulatorTraits<Emu64>::PVOID> info{c.emu, process_information};
            info.write(0);

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessDefaultHardErrorMode || info_class == ProcessWx86Information)
        {
            if (return_length)
            {
                return_length.write(sizeof(ULONG));
            }

            if (process_information_length != sizeof(ULONG))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<ULONG> info{c.emu, process_information};
            info.write(0);

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessEnclaveInformation || info_class == ProcessMitigationPolicy)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == ProcessTimes)
        {
            if (return_length)
            {
                return_length.write(sizeof(KERNEL_USER_TIMES));
            }

            if (process_information_length != sizeof(KERNEL_USER_TIMES))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<KERNEL_USER_TIMES> info{c.emu, process_information};
            info.write(KERNEL_USER_TIMES{});

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessBasicInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(PROCESS_BASIC_INFORMATION64));
            }

            if (process_information_length != sizeof(PROCESS_BASIC_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<PROCESS_BASIC_INFORMATION64> info{c.emu, process_information};
            info.access([&](PROCESS_BASIC_INFORMATION64& basic_info) {
                basic_info.PebBaseAddress = c.proc.peb.ptr();
                basic_info.UniqueProcessId = 1;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessImageFileNameWin32)
        {
            const auto peb = c.proc.peb.read();
            emulator_object<RTL_USER_PROCESS_PARAMETERS64> proc_params{c.emu, peb.ProcessParameters};
            const auto params = proc_params.read();
            const auto length = params.ImagePathName.Length + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + 2;

            if (return_length)
            {
                return_length.write(static_cast<uint32_t>(length));
            }

            if (process_information_length < length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> info{c.emu, process_information};
            info.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
                const auto buffer_start =
                    static_cast<uint64_t>(process_information) + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
                const auto string = read_unicode_string(c.emu, params.ImagePathName);
                c.emu.write_memory(buffer_start, string.c_str(), (string.size() + 1) * 2);
                str.Length = params.ImagePathName.Length;
                str.MaximumLength = str.Length;
                str.Buffer = buffer_start;
            });

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported process info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, const handle thread_handle,
                                             const uint32_t info_class, const uint64_t thread_information,
                                             const uint32_t thread_information_length,
                                             const emulator_object<uint32_t> return_length)
    {
        const auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == ThreadBasicInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(THREAD_BASIC_INFORMATION64));
            }

            if (thread_information_length != sizeof(THREAD_BASIC_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_BASIC_INFORMATION64> info{c.emu, thread_information};
            info.access([&](THREAD_BASIC_INFORMATION64& i) {
                i.TebBaseAddress = thread->teb->ptr();
                i.ClientId = thread->teb->read().ClientId;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadAmILastThread)
        {
            if (return_length)
            {
                return_length.write(sizeof(ULONG));
            }

            if (thread_information_length != sizeof(ULONG))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<ULONG> info{c.emu, thread_information};
            info.write(c.proc.threads.size() <= 1);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadQuerySetWin32StartAddress)
        {
            if (return_length)
            {
                return_length.write(sizeof(EmulatorTraits<Emu64>::PVOID));
            }

            if (thread_information_length != sizeof(EmulatorTraits<Emu64>::PVOID))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<EmulatorTraits<Emu64>::PVOID> info{c.emu, thread_information};
            info.write(thread->start_address);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported thread query info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, const handle file_handle,
                                         const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                         const uint64_t file_information, const ULONG length,
                                         const FILE_INFORMATION_CLASS info_class)
    {
        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == FilePositionInformation)
        {
            if (!f->handle)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = sizeof(FILE_POSITION_INFORMATION);
                io_status_block.write(block);
            }

            if (length != sizeof(FILE_POSITION_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<FILE_POSITION_INFORMATION> info{c.emu, file_information};
            const auto i = info.read();

            if (!f->handle.seek_to(i.CurrentByteOffset.QuadPart))
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported set file info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    std::vector<file_entry> scan_directory(const std::filesystem::path& dir)
    {
        std::vector<file_entry> files{
            {"."},
            {".."},
        };

        for (const auto& file : std::filesystem::directory_iterator(dir))
        {
            files.emplace_back(file_entry{
                .file_path = file.path().filename(),
            });
        }

        return files;
    }

    template <typename T>
    NTSTATUS handle_file_enumeration(const syscall_context& c,
                                     const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                     const uint64_t file_information, const uint32_t length, const ULONG query_flags,
                                     file* f)
    {
        if (!f->enumeration_state || query_flags & SL_RESTART_SCAN)
        {
            f->enumeration_state.emplace(file_enumeration_state{});
            f->enumeration_state->files = scan_directory(f->name);
        }

        auto& enum_state = *f->enumeration_state;

        size_t current_offset{0};
        emulator_object<T> object{c.emu};

        size_t current_index = enum_state.current_index;

        do
        {
            if (current_index >= enum_state.files.size())
            {
                break;
            }

            const auto new_offset = align_up(current_offset, 8);
            const auto& current_file = enum_state.files[current_index];
            const auto file_name = current_file.file_path.u16string();
            const auto required_size = sizeof(T) + (file_name.size() * 2) - 2;
            const auto end_offset = new_offset + required_size;

            if (end_offset > length)
            {
                if (current_offset == 0)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = end_offset;
                    io_status_block.write(block);

                    return STATUS_BUFFER_OVERFLOW;
                }

                break;
            }

            if (object)
            {
                const auto object_offset = object.value() - file_information;

                object.access(
                    [&](T& dir_info) { dir_info.NextEntryOffset = static_cast<ULONG>(new_offset - object_offset); });
            }

            T info{};
            info.NextEntryOffset = 0;
            info.FileIndex = static_cast<ULONG>(current_index);
            info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            info.FileNameLength = static_cast<ULONG>(file_name.size() * 2);

            object.set_address(file_information + new_offset);
            object.write(info);

            c.emu.write_memory(object.value() + offsetof(T, FileName), file_name.data(), info.FileNameLength);

            ++current_index;
            current_offset = end_offset;
        } while ((query_flags & SL_RETURN_SINGLE_ENTRY) == 0);

        if ((query_flags & SL_NO_CURSOR_UPDATE) == 0)
        {
            enum_state.current_index = current_index;
        }

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
        block.Information = current_offset;
        io_status_block.write(block);

        return current_index < enum_state.files.size() ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
    }

    NTSTATUS handle_NtQueryDirectoryFileEx(
        const syscall_context& c, const handle file_handle, const handle /*event_handle*/,
        const emulator_pointer /*PIO_APC_ROUTINE*/ /*apc_routine*/, const emulator_pointer /*apc_context*/,
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t file_information,
        const uint32_t length, const uint32_t info_class, const ULONG query_flags,
        const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*file_name*/)
    {
        auto* f = c.proc.files.get(file_handle);
        if (!f || !f->is_directory())
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == FileDirectoryInformation)
        {
            return handle_file_enumeration<FILE_DIRECTORY_INFORMATION>(c, io_status_block, file_information, length,
                                                                       query_flags, f);
        }

        if (info_class == FileFullDirectoryInformation)
        {
            return handle_file_enumeration<FILE_FULL_DIR_INFORMATION>(c, io_status_block, file_information, length,
                                                                      query_flags, f);
        }

        if (info_class == FileBothDirectoryInformation)
        {
            return handle_file_enumeration<FILE_BOTH_DIR_INFORMATION>(c, io_status_block, file_information, length,
                                                                      query_flags, f);
        }

        c.win_emu.log.error("Unsupported query directory file info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInformationFile(
        const syscall_context& c, const handle file_handle,
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t file_information,
        const uint32_t length, const uint32_t info_class)
    {
        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == FileNameInformation)
        {
            const auto required_length = sizeof(FILE_NAME_INFORMATION) + (f->name.size() * 2);

            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = sizeof(FILE_NAME_INFORMATION) + required_length;
                io_status_block.write(block);
            }

            if (length != required_length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            c.emu.write_memory(file_information, FILE_NAME_INFORMATION{
                                                     .FileNameLength = static_cast<ULONG>(f->name.size() * 2),
                                                     .FileName = {},
                                                 });

            c.emu.write_memory(file_information + offsetof(FILE_NAME_INFORMATION, FileName), f->name.c_str(),
                               (f->name.size() + 1) * 2);

            return STATUS_SUCCESS;
        }

        if (info_class == FileStandardInformation)
        {
            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = sizeof(FILE_STANDARD_INFORMATION);
                io_status_block.write(block);
            }

            if (length != sizeof(FILE_STANDARD_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<FILE_STANDARD_INFORMATION> info{c.emu, file_information};
            FILE_STANDARD_INFORMATION i{};
            i.Directory = f->is_directory() ? TRUE : FALSE;

            if (f->handle)
            {
                i.EndOfFile.QuadPart = f->handle.size();
            }

            info.write(i);

            return STATUS_SUCCESS;
        }

        if (info_class == FilePositionInformation)
        {
            if (!f->handle)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = sizeof(FILE_POSITION_INFORMATION);
                io_status_block.write(block);
            }

            if (length != sizeof(FILE_POSITION_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<FILE_POSITION_INFORMATION> info{c.emu, file_information};
            FILE_POSITION_INFORMATION i{};

            i.CurrentByteOffset.QuadPart = f->handle.tell();

            info.write(i);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported query file info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, const handle process_handle,
                                            const uint32_t info_class, const uint64_t process_information,
                                            const uint32_t process_information_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == ProcessSchedulerSharedData || info_class == ProcessConsoleHostProcess ||
            info_class == ProcessFaultInformation || info_class == ProcessDefaultHardErrorMode ||
            info_class == ProcessRaiseUMExceptionOnInvalidHandleClose)
        {
            return STATUS_SUCCESS;
        }

        if (info_class == ProcessTlsInformation)
        {
            constexpr auto thread_data_offset = offsetof(PROCESS_TLS_INFO, ThreadData);
            if (process_information_length < thread_data_offset)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_TLS_INFO> data{c.emu, process_information + thread_data_offset};

            PROCESS_TLS_INFO tls_info{};
            c.emu.read_memory(process_information, &tls_info, thread_data_offset);

            for (uint32_t i = 0; i < tls_info.ThreadDataCount; ++i)
            {
                auto entry = data.read(i);

                const auto _ = utils::finally([&] { data.write(entry, i); });

                if (i >= c.proc.threads.size())
                {
                    entry.Flags = 0;
                    continue;
                }

                auto thread_iterator = c.proc.threads.begin();
                std::advance(thread_iterator, i);

                entry.Flags = 2;

                thread_iterator->second.teb->access([&](TEB64& teb) {
                    entry.ThreadId = teb.ClientId.UniqueThread;

                    const auto tls_vector = teb.ThreadLocalStoragePointer;

                    if (tls_info.TlsRequest == ProcessTlsReplaceIndex)
                    {
                        const auto tls_entry_ptr = tls_vector + tls_info.TlsIndex;

                        const auto old_entry = c.emu.read_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr);
                        c.emu.write_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr, entry.TlsModulePointer);

                        entry.TlsModulePointer = old_entry;
                    }
                    else if (tls_info.TlsRequest == ProcessTlsReplaceVector)
                    {
                        const auto new_tls_vector = entry.TlsVector;

                        for (uint32_t index = 0; index < tls_info.TlsVectorLength; ++index)
                        {
                            const auto old_entry = c.emu.read_memory<void*>(tls_vector + index);
                            c.emu.write_memory<void*>(new_tls_vector + index, old_entry);
                        }

                        teb.ThreadLocalStoragePointer = new_tls_vector;
                        entry.TlsVector = tls_vector;
                    }
                });
            }

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported info process class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSetInformationKey()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtApphelpCacheControl()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, const handle process_handle,
                                           const emulator_object<uint64_t> base_address,
                                           const emulator_object<uint32_t> bytes_to_protect, const uint32_t protection,
                                           const emulator_object<uint32_t> old_protection)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto orig_start = base_address.read();
        const auto orig_length = bytes_to_protect.read();

        const auto aligned_start = page_align_down(orig_start);
        const auto aligned_length = page_align_up(orig_start + orig_length) - aligned_start;

        base_address.write(aligned_start);
        bytes_to_protect.write(static_cast<uint32_t>(aligned_length));

        const auto requested_protection = map_nt_to_emulator_protection(protection);

        c.win_emu.log.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n",
                            aligned_start, aligned_start + aligned_length,
                            get_permission_string(requested_protection).c_str());

        memory_permission old_protection_value{};

        try
        {
            c.emu.protect_memory(aligned_start, aligned_length, requested_protection, &old_protection_value);
        }
        catch (...)
        {
            return STATUS_INVALID_ADDRESS;
        }

        const auto current_protection = map_emulator_to_nt_protection(old_protection_value);
        old_protection.write(current_protection);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenDirectoryObject(
        const syscall_context& c, const emulator_object<handle> directory_handle, const ACCESS_MASK /*desired_access*/,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto object_name =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));

        if (object_name == u"\\KnownDlls")
        {
            directory_handle.write(KNOWN_DLLS_DIRECTORY);
            return STATUS_SUCCESS;
        }

        if (object_name == u"\\Sessions\\1\\BaseNamedObjects")
        {
            directory_handle.write(BASE_NAMED_OBJECTS_DIRECTORY);
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenSymbolicLinkObject(
        const syscall_context& c, const emulator_object<handle> link_handle, ACCESS_MASK /*desired_access*/,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto object_name =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));

        if (object_name == u"KnownDllPath")
        {
            link_handle.write(KNOWN_DLLS_SYMLINK);
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQuerySymbolicLinkObject(const syscall_context& c, const handle link_handle,
                                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> link_target,
                                              const emulator_object<ULONG> returned_length)
    {
        if (link_handle == KNOWN_DLLS_SYMLINK)
        {
            constexpr std::u16string_view system32 = u"C:\\WINDOWS\\System32";
            constexpr auto str_length = system32.size() * 2;
            constexpr auto max_length = str_length + 2;

            returned_length.write(max_length);

            bool too_small = false;
            link_target.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
                if (str.MaximumLength < max_length)
                {
                    too_small = true;
                    return;
                }

                str.Length = str_length;
                c.emu.write_memory(str.Buffer, system32.data(), max_length);
            });

            return too_small ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, const handle process_handle,
                                              const emulator_object<uint64_t> base_address,
                                              const emulator_object<uint64_t> bytes_to_allocate,
                                              const uint32_t allocation_type, const uint32_t page_protection)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        auto allocation_bytes = bytes_to_allocate.read();
        allocation_bytes = page_align_up(allocation_bytes);
        bytes_to_allocate.write(allocation_bytes);

        const auto protection = map_nt_to_emulator_protection(page_protection);

        auto potential_base = base_address.read();
        if (!potential_base)
        {
            potential_base = c.emu.find_free_allocation_base(allocation_bytes);
        }

        if (!potential_base)
        {
            return STATUS_MEMORY_NOT_ALLOCATED;
        }

        base_address.write(potential_base);

        const bool reserve = allocation_type & MEM_RESERVE;
        const bool commit = allocation_type & MEM_COMMIT;

        if ((allocation_type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)) || (!commit && !reserve))
        {
            throw std::runtime_error("Unsupported allocation type!");
        }

        if (commit && !reserve && c.emu.commit_memory(potential_base, allocation_bytes, protection))
        {
            return STATUS_SUCCESS;
        }

        return c.emu.allocate_memory(potential_base, allocation_bytes, protection, !commit)
                   ? STATUS_SUCCESS
                   : STATUS_MEMORY_NOT_ALLOCATED;
    }

    NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, const handle process_handle,
                                            const emulator_object<uint64_t> base_address, const uint64_t /*zero_bits*/,
                                            const emulator_object<uint64_t> bytes_to_allocate,
                                            const uint32_t allocation_type, const uint32_t page_protection)
    {
        return handle_NtAllocateVirtualMemoryEx(c, process_handle, base_address, bytes_to_allocate, allocation_type,
                                                page_protection);
    }

    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, const handle process_handle,
                                        const emulator_object<uint64_t> base_address,
                                        const emulator_object<uint64_t> bytes_to_allocate, const uint32_t free_type)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto allocation_base = base_address.read();
        const auto allocation_size = bytes_to_allocate.read();

        if (free_type & MEM_RELEASE)
        {
            return c.emu.release_memory(allocation_base, allocation_size) ? STATUS_SUCCESS
                                                                          : STATUS_MEMORY_NOT_ALLOCATED;
        }

        if (free_type & MEM_DECOMMIT)
        {
            return c.emu.decommit_memory(allocation_base, allocation_size) ? STATUS_SUCCESS
                                                                           : STATUS_MEMORY_NOT_ALLOCATED;
        }

        throw std::runtime_error("Bad free type");
    }

    NTSTATUS handle_NtCreateSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                    const ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    const emulator_object<ULARGE_INTEGER> maximum_size,
                                    const ULONG section_page_protection, const ULONG allocation_attributes,
                                    const handle file_handle)
    {
        section s{};
        s.section_page_protection = section_page_protection;
        s.allocation_attributes = allocation_attributes;

        const auto* file = c.proc.files.get(file_handle);
        if (file)
        {
            c.win_emu.log.print(color::dark_gray, "--> Section for file %s\n", u16_to_u8(file->name).c_str());
            s.file_name = file->name;
        }

        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                const auto name = read_unicode_string(
                    c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));
                c.win_emu.log.print(color::dark_gray, "--> Section with name %s\n", u16_to_u8(name).c_str());
                s.name = std::move(name);
            }
        }

        if (maximum_size)
        {
            maximum_size.access([&](ULARGE_INTEGER& large_int) {
                large_int.QuadPart = page_align_up(large_int.QuadPart);
                s.maximum_size = large_int.QuadPart;
            });
        }
        else if (!file)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto h = c.proc.sections.store(std::move(s));
        section_handle.write(h);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtConnectPort(const syscall_context& c, const emulator_object<handle> client_port_handle,
                                  const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                  const emulator_object<SECURITY_QUALITY_OF_SERVICE> /*security_qos*/,
                                  const emulator_object<PORT_VIEW64> client_shared_memory,
                                  const emulator_object<REMOTE_PORT_VIEW64> /*server_shared_memory*/,
                                  const emulator_object<ULONG> /*maximum_message_length*/,
                                  const emulator_pointer connection_info,
                                  const emulator_object<ULONG> connection_info_length)
    {
        auto port_name = read_unicode_string(c.emu, server_port_name);
        c.win_emu.log.print(color::dark_gray, "NtConnectPort: %s\n", u16_to_u8(port_name).c_str());

        port p{};
        p.name = std::move(port_name);

        if (connection_info)
        {
            std::vector<uint8_t> zero_mem{};
            zero_mem.resize(connection_info_length.read(), 0);
            c.emu.write_memory(connection_info, zero_mem.data(), zero_mem.size());
        }

        client_shared_memory.access([&](PORT_VIEW64& view) {
            p.view_base = c.emu.allocate_memory(view.ViewSize, memory_permission::read_write);
            view.ViewBase = p.view_base;
            view.ViewRemoteBase = view.ViewBase;
        });

        const auto handle = c.proc.ports.store(std::move(p));
        client_port_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, const handle process_handle,
                                        const emulator_pointer base_address, const emulator_pointer buffer,
                                        const ULONG number_of_bytes_to_read,
                                        const emulator_object<ULONG> number_of_bytes_read)
    {
        number_of_bytes_read.write(0);

        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        std::vector<uint8_t> memory{};
        memory.resize(number_of_bytes_read);

        if (!c.emu.try_read_memory(base_address, memory.data(), memory.size()))
        {
            return STATUS_INVALID_ADDRESS;
        }

        c.emu.write_memory(buffer, memory.data(), memory.size());
        number_of_bytes_read.write(number_of_bytes_to_read);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtDeviceIoControlFile(const syscall_context& c, const handle file_handle, const handle event,
                                          const emulator_pointer /*PIO_APC_ROUTINE*/ apc_routine,
                                          const emulator_pointer apc_context,
                                          const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                          const ULONG io_control_code, const emulator_pointer input_buffer,
                                          const ULONG input_buffer_length, const emulator_pointer output_buffer,
                                          const ULONG output_buffer_length)
    {
        auto* device = c.proc.devices.get(file_handle);
        if (!device)
        {
            return STATUS_INVALID_HANDLE;
        }

        io_device_context context{c.emu};
        context.event = event;
        context.apc_routine = apc_routine;
        context.apc_context = apc_context;
        context.io_status_block = io_status_block;
        context.io_control_code = io_control_code;
        context.input_buffer = input_buffer;
        context.input_buffer_length = input_buffer_length;
        context.output_buffer = output_buffer;
        context.output_buffer_length = output_buffer_length;

        return device->execute_ioctl(c.win_emu, context);
    }

    NTSTATUS handle_NtQueryWnfStateData()
    {
        // puts("NtQueryWnfStateData not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryWnfStateNameInformation()
    {
        // puts("NtQueryWnfStateNameInformation not supported");
        // return STATUS_NOT_SUPPORTED;
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenThreadToken(const syscall_context&, const handle thread_handle,
                                      const ACCESS_MASK /*desired_access*/, const BOOLEAN /*open_as_self*/,
                                      const emulator_object<handle> token_handle)
    {
        if (thread_handle != CURRENT_THREAD)
        {
            return STATUS_NOT_SUPPORTED;
        }

        token_handle.write(CURRENT_THREAD_TOKEN);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenThreadTokenEx(const syscall_context& c, const handle thread_handle,
                                        const ACCESS_MASK desired_access, const BOOLEAN open_as_self,
                                        const ULONG /*handle_attributes*/, const emulator_object<handle> token_handle)
    {
        return handle_NtOpenThreadToken(c, thread_handle, desired_access, open_as_self, token_handle);
    }

    NTSTATUS handle_NtOpenProcessToken(const syscall_context&, const handle process_handle,
                                       const ACCESS_MASK /*desired_access*/, const emulator_object<handle> token_handle)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        token_handle.write(CURRENT_PROCESS_TOKEN);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenProcessTokenEx(const syscall_context& c, const handle process_handle,
                                         const ACCESS_MASK desired_access, const ULONG /*handle_attributes*/,
                                         const emulator_object<handle> token_handle)
    {
        return handle_NtOpenProcessToken(c, process_handle, desired_access, token_handle);
    }

    NTSTATUS handle_NtQuerySecurityAttributesToken()
    {
        // puts("NtQuerySecurityAttributesToken not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryLicenseValue()
    {
        // puts("NtQueryLicenseValue not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtTestAlert()
    {
        // puts("NtTestAlert not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserSystemParametersInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    TOKEN_TYPE get_token_type(const handle token_handle)
    {
        return token_handle == DUMMY_IMPERSONATION_TOKEN //
                   ? TokenImpersonation
                   : TokenPrimary;
    }

    NTSTATUS handle_NtDuplicateToken(const syscall_context&, const handle existing_token_handle,
                                     ACCESS_MASK /*desired_access*/,
                                     const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                     /*object_attributes*/,
                                     const BOOLEAN /*effective_only*/, const TOKEN_TYPE type,
                                     const emulator_object<handle> new_token_handle)
    {
        if (get_token_type(existing_token_handle) == type)
        {
            new_token_handle.write(existing_token_handle);
        }
        else if (type == TokenPrimary)
        {
            new_token_handle.write(CURRENT_PROCESS_TOKEN);
        }
        else
        {
            new_token_handle.write(DUMMY_IMPERSONATION_TOKEN);
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryTimerResolution(const syscall_context&, const emulator_object<ULONG> maximum_time,
                                           const emulator_object<ULONG> minimum_time,
                                           const emulator_object<ULONG> current_time)
    {
        maximum_time.write_if_valid(0x0002625a);
        minimum_time.write_if_valid(0x00001388);
        current_time.write_if_valid(0x00002710);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryInformationToken(const syscall_context& c, const handle token_handle,
                                            const TOKEN_INFORMATION_CLASS token_information_class,
                                            const uint64_t token_information, const ULONG token_information_length,
                                            const emulator_object<ULONG> return_length)
    {
        if (token_handle != CURRENT_PROCESS_TOKEN && token_handle != CURRENT_THREAD_TOKEN &&
            token_handle != CURRENT_THREAD_EFFECTIVE_TOKEN && token_handle != DUMMY_IMPERSONATION_TOKEN)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const uint8_t sid[] = {
            0x01, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x15, 0x00, 0x00, 0x00, 0x84, 0x94,
            0xD4, 0x04, 0x4B, 0x68, 0x42, 0x34, 0x23, 0xBE, 0x69, 0x4E, 0xE9, 0x03, 0x00, 0x00,
        };

        if (token_information_class == TokenUser)
        {
            constexpr auto required_size = sizeof(sid) + 0x10;
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_USER64 user{};
            user.User.Attributes = 0;
            user.User.Sid = token_information + 0x10;

            emulator_object<TOKEN_USER64>{c.emu, token_information}.write(user);
            c.emu.write_memory(token_information + 0x10, sid, sizeof(sid));
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenType)
        {
            constexpr auto required_size = sizeof(TOKEN_TYPE);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_object<TOKEN_TYPE>{c.emu, token_information}.write(get_token_type(token_handle));
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenSessionId)
        {
            constexpr auto required_size = sizeof(ULONG);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_object<ULONG>{c.emu, token_information}.write(1);
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenPrivateNameSpace)
        {
            constexpr auto required_size = sizeof(ULONG);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_object<ULONG>{c.emu, token_information}.write(0);
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenUIAccess)
        {
            constexpr auto required_size = sizeof(ULONG);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_object<ULONG>{c.emu, token_information}.write(1);
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenElevation)
        {
            constexpr auto required_size = sizeof(TOKEN_ELEVATION);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(token_information, TOKEN_ELEVATION{
                                                      .TokenIsElevated = 0,
                                                  });
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenIsAppContainer)
        {
            constexpr auto required_size = sizeof(ULONG);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            emulator_object<ULONG>{c.emu, token_information}.write(0);
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenStatistics)
        {
            constexpr auto required_size = sizeof(TOKEN_STATISTICS);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(token_information, TOKEN_STATISTICS{});

            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenSecurityAttributes)
        {
            constexpr auto required_size = sizeof(TOKEN_SECURITY_ATTRIBUTES_INFORMATION);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(token_information, TOKEN_SECURITY_ATTRIBUTES_INFORMATION{
                                                      .Version = 0,
                                                      .Reserved = {},
                                                      .AttributeCount = 0,
                                                      .Attribute = {},
                                                  });

            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenIntegrityLevel)
        {
            constexpr auto required_size = sizeof(sid) + sizeof(TOKEN_MANDATORY_LABEL64);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            TOKEN_MANDATORY_LABEL64 label{};
            label.Label.Attributes = 0;
            label.Label.Sid = token_information + sizeof(TOKEN_MANDATORY_LABEL64);

            emulator_object<TOKEN_MANDATORY_LABEL64>{c.emu, token_information}.write(label);
            c.emu.write_memory(token_information + sizeof(TOKEN_MANDATORY_LABEL64), sid, sizeof(sid));
            return STATUS_SUCCESS;
        }

        if (token_information_class == TokenBnoIsolation)
        {
            constexpr auto required_size = sizeof(TOKEN_BNO_ISOLATION_INFORMATION64);
            return_length.write(required_size);

            if (required_size > token_information_length)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            c.emu.write_memory(token_information, TOKEN_BNO_ISOLATION_INFORMATION64{
                                                      .IsolationPrefix = 0,
                                                      .IsolationEnabled = FALSE,
                                                  });

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported token info class: %X\n", token_information_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtDxgkIsFeatureEnabled()
    {
        // puts("NtDxgkIsFeatureEnabled not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInstallUILanguage()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo()
    {
        // puts("NtUserDisplayConfigGetDeviceInfo not supported");
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGdiInit(const syscall_context& c)
    {
        c.proc.peb.access([&](PEB64& peb) {
            if (!peb.GdiSharedHandleTable)
            {
                peb.GdiSharedHandleTable = reinterpret_cast<EmulatorTraits<Emu64>::PVOID*>(
                    c.proc.base_allocator.reserve<GDI_SHARED_MEMORY64>().ptr());
            }
        });

        return STATUS_WAIT_1;
    }

    NTSTATUS handle_NtGdiInit2(const syscall_context& c)
    {
        handle_NtGdiInit(c);
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGetMUIRegistryInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserRegisterWindowMessage()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetThreadState()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtIsUILanguageComitted()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUpdateWnfStateData()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGetNlsSectionPtr()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, const handle port_handle, const ULONG /*flags*/,
                                              const emulator_object<PORT_MESSAGE64> /*send_message*/,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*send_message_attributes*/
                                              ,
                                              const emulator_object<PORT_MESSAGE64> receive_message,
                                              const emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*receive_message_attributes*/,
                                              const emulator_object<LARGE_INTEGER> /*timeout*/)
    {
        const auto* port = c.proc.ports.get(port_handle);
        if (!port)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (port->name != u"\\Windows\\ApiPort")
        {
            puts("!!! BAD PORT");
            return STATUS_NOT_SUPPORTED;
        }

        // TODO: Fix this. This is broken and wrong.

        const emulator_object<PORT_DATA_ENTRY<EmulatorTraits<Emu64>>> data{c.emu, receive_message.value() + 0x48};
        const auto dest = data.read();
        const auto base = dest.Base;

        const auto value = base + 0x10;
        c.emu.write_memory(base + 8, &value, sizeof(value));

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtInitializeNlsFiles(const syscall_context& c, const emulator_object<uint64_t> base_address,
                                         const emulator_object<LCID> default_locale_id,
                                         const emulator_object<LARGE_INTEGER> /*default_casing_table_size*/)
    {
        const auto locale_file = utils::io::read_file(R"(C:\Windows\System32\locale.nls)");
        if (locale_file.empty())
        {
            return STATUS_FILE_INVALID;
        }

        const auto size = page_align_up(locale_file.size());
        const auto base = c.emu.allocate_memory(size, memory_permission::read);
        c.emu.write_memory(base, locale_file.data(), locale_file.size());

        base_address.write(base);
        default_locale_id.write(0x407);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtContinue(const syscall_context& c, const emulator_object<CONTEXT64> thread_context,
                               const BOOLEAN /*raise_alert*/)
    {
        c.write_status = false;

        const auto context = thread_context.read();
        context_frame::restore(c.emu, context);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtTerminateProcess(const syscall_context& c, const handle process_handle, NTSTATUS exit_status)
    {
        if (process_handle == 0)
        {
            for (auto& thread : c.proc.threads | std::views::values)
            {
                if (&thread != c.proc.active_thread)
                {
                    thread.exit_status = exit_status;
                }
            }

            return STATUS_SUCCESS;
        }

        if (process_handle == CURRENT_PROCESS)
        {
            c.proc.exit_status = exit_status;
            c.emu.stop();
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtReadFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/,
                               const uint64_t /*apc_routine*/, const uint64_t /*apc_context*/,
                               const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                               const uint64_t buffer, const ULONG length,
                               const emulator_object<LARGE_INTEGER> /*byte_offset*/,
                               const emulator_object<ULONG> /*key*/)
    {
        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        std::string temp_buffer{};
        temp_buffer.resize(length);

        const auto bytes_read = fread(temp_buffer.data(), 1, temp_buffer.size(), f->handle);

        if (io_status_block)
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = bytes_read;
            io_status_block.write(block);
        }

        c.emu.write_memory(buffer, temp_buffer.data(), temp_buffer.size());
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtWriteFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/,
                                const uint64_t /*apc_routine*/, const uint64_t /*apc_context*/,
                                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                const uint64_t buffer, const ULONG length,
                                const emulator_object<LARGE_INTEGER> /*byte_offset*/,
                                const emulator_object<ULONG> /*key*/)
    {
        std::string temp_buffer{};
        temp_buffer.resize(length);
        c.emu.read_memory(buffer, temp_buffer.data(), temp_buffer.size());

        if (file_handle == STDOUT_HANDLE)
        {
            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = length;
                io_status_block.write(block);
            }

            if (!temp_buffer.ends_with("\n"))
            {
                temp_buffer.push_back('\n');
            }

            c.win_emu.on_stdout(temp_buffer);
            c.win_emu.log.info("%.*s", static_cast<int>(temp_buffer.size()), temp_buffer.data());

            return STATUS_SUCCESS;
        }

        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto bytes_written = fwrite(temp_buffer.data(), 1, temp_buffer.size(), f->handle);

        if (io_status_block)
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = bytes_written;
            io_status_block.write(block);
        }

        return STATUS_SUCCESS;
    }

    constexpr std::u16string map_mode(const ACCESS_MASK desired_access, const ULONG create_disposition)
    {
        std::u16string mode{};

        switch (create_disposition)
        {
        case FILE_CREATE:
        case FILE_SUPERSEDE:
            if (desired_access & GENERIC_WRITE)
            {
                mode = u"wb";
            }
            break;

        case FILE_OPEN:
        case FILE_OPEN_IF:
            if (desired_access & GENERIC_WRITE)
            {
                mode = u"r+b";
            }
            else if (desired_access & GENERIC_READ || desired_access & SYNCHRONIZE)
            {
                mode = u"rb";
            }
            break;

        case FILE_OVERWRITE:
        case FILE_OVERWRITE_IF:
            if (desired_access & GENERIC_WRITE)
            {
                mode = u"w+b";
            }
            break;

        default:
            mode = u"";
            break;
        }

        if (desired_access & FILE_APPEND_DATA)
        {
            mode = u"a+b";
        }

        return mode;
    }

    NTSTATUS handle_NtCreateFile(const syscall_context& c, const emulator_object<handle> file_handle,
                                 ACCESS_MASK desired_access,
                                 const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                 const emulator_object<LARGE_INTEGER> /*allocation_size*/, ULONG /*file_attributes*/,
                                 ULONG /*share_access*/, ULONG create_disposition, ULONG create_options,
                                 uint64_t ea_buffer, ULONG ea_length)
    {
        const auto attributes = object_attributes.read();
        auto filename =
            read_unicode_string(c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));

        auto printer = utils::finally(
            [&] { c.win_emu.log.print(color::dark_gray, "--> Opening file: %s\n", u16_to_u8(filename).c_str()); });

        constexpr std::u16string_view device_prefix = u"\\Device\\";
        if (filename.starts_with(device_prefix))
        {
            const io_device_creation_data data{
                .buffer = ea_buffer,
                .length = ea_length,
            };

            auto device_name = filename.substr(device_prefix.size());
            io_device_container container{std::move(device_name), c.win_emu, data};

            const auto handle = c.proc.devices.store(std::move(container));
            file_handle.write(handle);

            return STATUS_SUCCESS;
        }

        handle root_handle{};
        root_handle.bits = attributes.RootDirectory;
        if (root_handle.value.is_pseudo && (filename == u"\\Reference" || filename == u"\\Connect"))
        {
            file_handle.write(root_handle);
            return STATUS_SUCCESS;
        }

        file f{};
        f.name = std::move(filename);

        if (attributes.RootDirectory)
        {
            const auto* root = c.proc.files.get(attributes.RootDirectory);
            if (!root)
            {
                return STATUS_INVALID_HANDLE;
            }

            f.name = root->name + f.name;
        }

        printer.cancel();

        if (f.name.ends_with(u"\\") || create_options & FILE_DIRECTORY_FILE)
        {
            c.win_emu.log.print(color::dark_gray, "--> Opening folder: %s\n", u16_to_u8(f.name).c_str());

            if (create_disposition & FILE_CREATE)
            {
                std::error_code ec{};
                std::filesystem::create_directory(f.name, ec);

                if (ec)
                {
                    return STATUS_ACCESS_DENIED;
                }
            }
            else if (!std::filesystem::is_directory(f.name))
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto handle = c.proc.files.store(std::move(f));
            file_handle.write(handle);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.print(color::dark_gray, "--> Opening file: %s\n", u16_to_u8(f.name).c_str());

        std::u16string mode = map_mode(desired_access, create_disposition);

        if (mode.empty())
        {
            return STATUS_NOT_SUPPORTED;
        }

        FILE* file{};

        const auto error = open_unicode(&file, f.name, mode);

        if (!file)
        {
            switch (error)
            {
            case ENOENT:
                return STATUS_OBJECT_NAME_NOT_FOUND;
            case EACCES:
                return STATUS_ACCESS_DENIED;
            case EISDIR:
                return STATUS_FILE_IS_A_DIRECTORY;
            default:
                return STATUS_NOT_SUPPORTED;
            }
        }

        f.handle = file;

        const auto handle = c.proc.files.store(std::move(f));
        file_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryAttributesFile(
        const syscall_context& c, const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
        const emulator_object<FILE_BASIC_INFORMATION> file_information)
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

        const auto filename = read_unicode_string(
            c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
        const auto u8_filename = u16_to_u8(filename);

        struct _stat64 file_stat{};
        if (_stat64(u8_filename.c_str(), &file_stat) != 0)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        file_information.access([&](FILE_BASIC_INFORMATION& info) {
            info.CreationTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastAccessTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastWriteTime = utils::convert_unix_to_windows_time(file_stat.st_mtime);
            info.ChangeTime = info.LastWriteTime;
            info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenFile(const syscall_context& c, const emulator_object<handle> file_handle,
                               const ACCESS_MASK desired_access,
                               const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                               const ULONG share_access, const ULONG open_options)
    {
        return handle_NtCreateFile(c, file_handle, desired_access, object_attributes, io_status_block, {c.emu}, 0,
                                   share_access, FILE_OPEN, open_options, 0, 0);
    }

    NTSTATUS handle_NtQueryObject(const syscall_context&, const handle /*handle*/,
                                  const OBJECT_INFORMATION_CLASS /*object_information_class*/,
                                  const emulator_pointer /*object_information*/,
                                  const ULONG /*object_information_length*/,
                                  const emulator_object<ULONG> /*return_length*/)
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInformationJobObject()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtSetSystemInformation()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtAccessCheck()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetKeyboardLayout()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtRaiseHardError(const syscall_context& c, const NTSTATUS error_status,
                                     const ULONG /*number_of_parameters*/,
                                     const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>
                                     /*unicode_string_parameter_mask*/,
                                     const emulator_object<DWORD> /*parameters*/,
                                     const HARDERROR_RESPONSE_OPTION /*valid_response_option*/,
                                     const emulator_object<HARDERROR_RESPONSE> response)
    {
        if (response)
        {
            response.write(ResponseAbort);
        }

        c.proc.exit_status = error_status;
        c.proc.exception_rip = c.emu.read_instruction_pointer();
        c.emu.stop();

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtRaiseException(const syscall_context& c,
                                     const emulator_object<EMU_EXCEPTION_RECORD<EmulatorTraits<Emu64>>>
                                     /*exception_record*/,
                                     const emulator_object<CONTEXT64> thread_context, const BOOLEAN handle_exception)
    {
        if (handle_exception)
        {
            puts("Unhandled exceptions not supported yet!");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        c.proc.exception_rip = thread_context.read().Rip;
        c.emu.stop();

        return STATUS_SUCCESS;
    }

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

        const auto name = read_unicode_string(
            c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});
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
                s.name = read_unicode_string(
                    c.emu, reinterpret_cast<UNICODE_STRING<EmulatorTraits<Emu64>>*>(attributes.ObjectName));
            }
        }

        if (!s.name.empty())
        {
            for (const auto& semaphore : c.proc.semaphores | std::views::values)
            {
                if (semaphore.name == s.name)
                {
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        const auto handle = c.proc.semaphores.store(std::move(s));
        semaphore_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAddAtomEx(const syscall_context& c, const uint64_t atom_name, const ULONG length,
                                const emulator_object<RTL_ATOM> atom, const ULONG /*flags*/)
    {
        std::wstring name{};
        name.resize(length / 2);

        c.emu.read_memory(atom_name, name.data(), length);

        uint16_t index = 0;
        if (!c.proc.atoms.empty())
        {
            auto i = c.proc.atoms.end();
            --i;
            index = i->first + 1;
        }

        std::optional<uint16_t> last_entry{};
        for (auto& entry : c.proc.atoms)
        {
            if (entry.second == name)
            {
                if (atom)
                {
                    atom.write(entry.first);
                    return STATUS_SUCCESS;
                }
            }

            if (entry.first > 0)
            {
                if (!last_entry)
                {
                    index = 0;
                }
                else
                {
                    const auto diff = entry.first - *last_entry;
                    if (diff > 1)
                    {
                        index = *last_entry + 1;
                    }
                }
            }

            last_entry = entry.first;
        }

        c.proc.atoms[index] = std::move(name);
        atom.write(index);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, const handle process_handle,
                                         const uint64_t base_address)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto* mod = c.proc.mod_manager.find_by_address(base_address);
        if (!mod)
        {
            puts("Unmapping non-module section not supported!");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        if (c.proc.mod_manager.unmap(base_address))
        {
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, const handle process_handle,
                                           const uint64_t base_address, const ULONG /*flags*/)
    {
        return handle_NtUnmapViewOfSection(c, process_handle, base_address);
    }

    NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, const emulator_object<handle> thread_handle,
                                     const ACCESS_MASK /*desired_access*/,
                                     const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                     /*object_attributes*/,
                                     const handle process_handle, const uint64_t start_routine, const uint64_t argument,
                                     const ULONG /*create_flags*/, const EmulatorTraits<Emu64>::SIZE_T /*zero_bits*/,
                                     const EmulatorTraits<Emu64>::SIZE_T stack_size,
                                     const EmulatorTraits<Emu64>::SIZE_T /*maximum_stack_size*/,
                                     const emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto h = c.proc.create_thread(c.emu, start_routine, argument, stack_size);
        thread_handle.write(h);

        if (!attribute_list)
        {
            return STATUS_SUCCESS;
        }

        const auto* thread = c.proc.threads.get(h);

        const emulator_object<PS_ATTRIBUTE<EmulatorTraits<Emu64>>> attributes{
            c.emu, attribute_list.value() + offsetof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>, Attributes)};

        const auto total_length = attribute_list.read().TotalLength;

        constexpr auto entry_size = sizeof(PS_ATTRIBUTE<EmulatorTraits<Emu64>>);
        constexpr auto header_size = sizeof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>) - entry_size;
        const auto attribute_count = (total_length - header_size) / entry_size;

        for (size_t i = 0; i < attribute_count; ++i)
        {
            attributes.access(
                [&](const PS_ATTRIBUTE<EmulatorTraits<Emu64>>& attribute) {
                    const auto type = attribute.Attribute & ~PS_ATTRIBUTE_THREAD;

                    if (type == PsAttributeClientId)
                    {
                        const auto client_id = thread->teb->read().ClientId;
                        write_attribute(c.emu, attribute, client_id);
                    }
                    else if (type == PsAttributeTebAddress)
                    {
                        write_attribute(c.emu, attribute, thread->teb->ptr());
                    }
                    else
                    {
                        c.win_emu.log.error("Unsupported thread attribute type: %" PRIx64 "\n", type);
                    }
                },
                i);
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryDebugFilterState()
    {
        return FALSE;
    }

    NTSTATUS handle_NtUserGetDpiForCurrentProcess()
    {
        return 96;
    }

    NTSTATUS handle_NtUserGetDCEx()
    {
        return 1;
    }

    NTSTATUS handle_NtUserModifyUserStartupInfoFlags()
    {
        return STATUS_SUCCESS;
    }

    bool is_awaitable_object_type(const handle h)
    {
        return h.value.type == handle_types::thread    //
               || h.value.type == handle_types::mutant //
               || h.value.type == handle_types::event;
    }

    NTSTATUS handle_NtWaitForMultipleObjects(const syscall_context& c, const ULONG count,
                                             const emulator_object<handle> handles, const WAIT_TYPE wait_type,
                                             const BOOLEAN alertable, const emulator_object<LARGE_INTEGER> timeout)
    {
        if (alertable)
        {
            c.win_emu.log.print(color::gray, "Alertable NtWaitForMultipleObjects not supported yet!\n");
        }

        if (wait_type != WaitAny && wait_type != WaitAll)
        {
            puts("Wait type not supported!");
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
        }

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(timeout.read());
        }

        c.win_emu.yield_thread();
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtWaitForSingleObject(const syscall_context& c, const handle h, const BOOLEAN alertable,
                                          const emulator_object<LARGE_INTEGER> timeout)
    {
        if (alertable)
        {
            c.win_emu.log.print(color::gray, "Alertable NtWaitForSingleObject not supported yet!\n");
        }

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
            t.await_time = utils::convert_delay_interval_to_time_point(timeout.read());
        }

        c.win_emu.yield_thread();
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtTerminateThread(const syscall_context& c, const handle thread_handle, const NTSTATUS exit_status)
    {
        auto* thread = !thread_handle.bits ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        thread->exit_status = exit_status;
        if (thread == c.proc.active_thread)
        {
            c.win_emu.yield_thread();
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtDelayExecution(const syscall_context& c, const BOOLEAN alertable,
                                     const emulator_object<LARGE_INTEGER> delay_interval)
    {
        if (alertable)
        {
            puts("Alertable NtDelayExecution not supported yet!");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        auto& t = c.win_emu.current_thread();
        t.await_time = utils::convert_delay_interval_to_time_point(delay_interval.read());

        c.win_emu.yield_thread();

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context& c, const uint64_t thread_id)
    {
        for (auto& t : c.proc.threads | std::views::values)
        {
            if (t.id == thread_id)
            {
                t.alerted = true;
                return STATUS_SUCCESS;
            }
        }

        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS handle_NtAlertThreadByThreadIdEx(const syscall_context& c, const uint64_t thread_id,
                                              const emulator_object<EMU_RTL_SRWLOCK<EmulatorTraits<Emu64>>> lock)
    {
        if (lock.value())
        {
            c.win_emu.log.print(color::gray, "NtAlertThreadByThreadIdEx with lock not supported yet!");
            // c.emu.stop();
            // return STATUS_NOT_SUPPORTED;
        }

        return handle_NtAlertThreadByThreadId(c, thread_id);
    }

    NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context& c, const uint64_t,
                                             const emulator_object<LARGE_INTEGER> timeout)
    {
        auto& t = c.win_emu.current_thread();
        t.waiting_for_alert = true;

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(timeout.read());
        }

        c.win_emu.yield_thread();

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtGetCurrentProcessorNumberEx(const syscall_context&,
                                                  const emulator_object<PROCESSOR_NUMBER> processor_number)
    {
        constexpr PROCESSOR_NUMBER number{};
        processor_number.write(number);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationVirtualMemory(const syscall_context&)
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtGetContextThread(const syscall_context& c, const handle thread_handle,
                                       const emulator_object<CONTEXT64> thread_context)
    {
        const auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        c.proc.active_thread->save(c.emu);
        const auto _ = utils::finally([&] { c.proc.active_thread->restore(c.emu); });

        thread->restore(c.emu);

        thread_context.access([&](CONTEXT64& context) {
            if (context.ContextFlags & CONTEXT_DEBUG_REGISTERS_64)
            {
                c.win_emu.log.print(color::pink, "--> Reading debug registers!\n");
            }

            context_frame::save(c.emu, context);
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtYieldExecution(const syscall_context& c)
    {
        c.win_emu.yield_thread();
        return STATUS_SUCCESS;
    }
}

void syscall_dispatcher::add_handlers(std::map<std::string, syscall_handler>& handler_mapping)
{
#define add_handler(syscall)                                                  \
    do                                                                        \
    {                                                                         \
        handler_mapping[#syscall] = make_syscall_handler<handle_##syscall>(); \
    } while (0)

    add_handler(NtSetInformationThread);
    add_handler(NtSetEvent);
    add_handler(NtClose);
    add_handler(NtOpenKey);
    add_handler(NtAllocateVirtualMemory);
    add_handler(NtQueryInformationProcess);
    add_handler(NtSetInformationProcess);
    add_handler(NtSetInformationVirtualMemory);
    add_handler(NtFreeVirtualMemory);
    add_handler(NtQueryVirtualMemory);
    add_handler(NtOpenThreadToken);
    add_handler(NtOpenThreadTokenEx);
    add_handler(NtQueryPerformanceCounter);
    add_handler(NtQuerySystemInformation);
    add_handler(NtCreateEvent);
    add_handler(NtProtectVirtualMemory);
    add_handler(NtOpenDirectoryObject);
    add_handler(NtTraceEvent);
    add_handler(NtAllocateVirtualMemoryEx);
    add_handler(NtCreateIoCompletion);
    add_handler(NtCreateWaitCompletionPacket);
    add_handler(NtCreateWorkerFactory);
    add_handler(NtManageHotPatch);
    add_handler(NtOpenSection);
    add_handler(NtMapViewOfSection);
    add_handler(NtOpenSymbolicLinkObject);
    add_handler(NtQuerySymbolicLinkObject);
    add_handler(NtQuerySystemInformationEx);
    add_handler(NtOpenFile);
    add_handler(NtQueryVolumeInformationFile);
    add_handler(NtApphelpCacheControl);
    add_handler(NtCreateSection);
    add_handler(NtConnectPort);
    add_handler(NtCreateFile);
    add_handler(NtDeviceIoControlFile);
    add_handler(NtQueryWnfStateData);
    add_handler(NtOpenProcessToken);
    add_handler(NtOpenProcessTokenEx);
    add_handler(NtQuerySecurityAttributesToken);
    add_handler(NtQueryLicenseValue);
    add_handler(NtTestAlert);
    add_handler(NtContinue);
    add_handler(NtTerminateProcess);
    add_handler(NtWriteFile);
    add_handler(NtRaiseHardError);
    add_handler(NtCreateSemaphore);
    add_handler(NtOpenSemaphore);
    add_handler(NtReadVirtualMemory);
    add_handler(NtQueryInformationToken);
    add_handler(NtDxgkIsFeatureEnabled);
    add_handler(NtAddAtomEx);
    add_handler(NtInitializeNlsFiles);
    add_handler(NtUnmapViewOfSection);
    add_handler(NtUnmapViewOfSectionEx);
    add_handler(NtDuplicateObject);
    add_handler(NtQueryInformationThread);
    add_handler(NtQueryWnfStateNameInformation);
    add_handler(NtAlpcSendWaitReceivePort);
    add_handler(NtGdiInit);
    add_handler(NtGdiInit2);
    add_handler(NtUserGetThreadState);
    add_handler(NtOpenKeyEx);
    add_handler(NtUserDisplayConfigGetDeviceInfo);
    add_handler(NtOpenEvent);
    add_handler(NtGetMUIRegistryInfo);
    add_handler(NtIsUILanguageComitted);
    add_handler(NtQueryInstallUILanguage);
    add_handler(NtUpdateWnfStateData);
    add_handler(NtRaiseException);
    add_handler(NtQueryInformationJobObject);
    add_handler(NtSetSystemInformation);
    add_handler(NtQueryInformationFile);
    add_handler(NtCreateThreadEx);
    add_handler(NtQueryDebugFilterState);
    add_handler(NtWaitForSingleObject);
    add_handler(NtTerminateThread);
    add_handler(NtDelayExecution);
    add_handler(NtWaitForAlertByThreadId);
    add_handler(NtAlertThreadByThreadIdEx);
    add_handler(NtAlertThreadByThreadId);
    add_handler(NtReadFile);
    add_handler(NtSetInformationFile);
    add_handler(NtUserRegisterWindowMessage);
    add_handler(NtQueryValueKey);
    add_handler(NtQueryKey);
    add_handler(NtGetNlsSectionPtr);
    add_handler(NtAccessCheck);
    add_handler(NtCreateKey);
    add_handler(NtNotifyChangeKey);
    add_handler(NtGetCurrentProcessorNumberEx);
    add_handler(NtQueryObject);
    add_handler(NtQueryAttributesFile);
    add_handler(NtWaitForMultipleObjects);
    add_handler(NtCreateMutant);
    add_handler(NtReleaseMutant);
    add_handler(NtDuplicateToken);
    add_handler(NtQueryTimerResolution);
    add_handler(NtSetInformationKey);
    add_handler(NtUserGetKeyboardLayout);
    add_handler(NtQueryDirectoryFileEx);
    add_handler(NtUserSystemParametersInfo);
    add_handler(NtGetContextThread);
    add_handler(NtYieldExecution);
    add_handler(NtUserModifyUserStartupInfoFlags);
    add_handler(NtUserGetDCEx);
    add_handler(NtUserGetDpiForCurrentProcess);

#undef add_handler
}
