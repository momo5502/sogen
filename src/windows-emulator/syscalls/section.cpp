#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/io.hpp>

namespace syscalls
{
    NTSTATUS handle_NtCreateSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                    const ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    const emulator_object<ULARGE_INTEGER> maximum_size, const ULONG section_page_protection,
                                    const ULONG allocation_attributes, const handle file_handle)
    {
        section s{};
        s.section_page_protection = section_page_protection;
        s.allocation_attributes = allocation_attributes;

        const auto* file = c.proc.files.get(file_handle);
        if (file)
        {
            c.win_emu.callbacks.on_generic_access("Section for file", file->name);
            s.file_name = file->name;
        }

        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                auto name = read_unicode_string(c.emu, attributes.ObjectName);
                c.win_emu.callbacks.on_generic_access("Section with name", name);
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

    NTSTATUS handle_NtOpenSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                  const ACCESS_MASK /*desired_access*/,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();

        auto filename = read_unicode_string(c.emu, attributes.ObjectName);
        c.win_emu.callbacks.on_generic_access("Opening section", filename);

        if (filename == u"\\Windows\\SharedSection")
        {
            constexpr auto shared_section_size = 0x10000;

            const auto address = c.win_emu.memory.find_free_allocation_base(shared_section_size);
            c.win_emu.memory.allocate_memory(address, shared_section_size, memory_permission::read_write);
            c.proc.shared_section_address = address;
            c.proc.shared_section_size = shared_section_size;

            section_handle.write(SHARED_SECTION);
            return STATUS_SUCCESS;
        }

        if (filename == u"DBWIN_BUFFER")
        {
            constexpr auto dbwin_buffer_section_size = 0x1000;

            const auto address = c.win_emu.memory.find_free_allocation_base(dbwin_buffer_section_size);
            c.win_emu.memory.allocate_memory(address, dbwin_buffer_section_size, memory_permission::read_write);
            c.proc.dbwin_buffer = address;
            c.proc.dbwin_buffer_size = dbwin_buffer_section_size;

            section_handle.write(DBWIN_BUFFER);
            return STATUS_SUCCESS;
        }

        if (filename == u"windows_shell_global_counters"             //
            || filename == u"Global\\__ComCatalogCache__"            //
            || filename == u"{00020000-0000-1005-8005-0000C06B5161}" //
            || filename == u"Global\\{00020000-0000-1005-8005-0000C06B5161}")
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (attributes.RootDirectory != KNOWN_DLLS_DIRECTORY && attributes.RootDirectory != BASE_NAMED_OBJECTS_DIRECTORY)
        {
            c.win_emu.log.error("Unsupported section\n");
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

    NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, const handle section_handle, const handle process_handle,
                                       const emulator_object<uint64_t> base_address,
                                       const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) /*zero_bits*/,
                                       const EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) /*commit_size*/,
                                       const emulator_object<LARGE_INTEGER> /*section_offset*/,
                                       const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                       const SECTION_INHERIT /*inherit_disposition*/, const ULONG /*allocation_type*/,
                                       const ULONG /*win32_protect*/)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_handle == SHARED_SECTION)
        {
            const auto shared_section_size = c.proc.shared_section_size;
            const auto address = c.proc.shared_section_address;

            const std::u16string_view windows_dir = c.proc.kusd.get().NtSystemRoot.arr;
            const auto windows_dir_size = windows_dir.size() * 2;

            constexpr auto windows_dir_offset = 0x10;
            c.emu.write_memory(address + 8, windows_dir_offset);

            // aka. BaseStaticServerData (BASE_STATIC_SERVER_DATA)
            const auto obj_address = address + windows_dir_offset;

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> windir_obj{c.emu, obj_address};
            windir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                const auto dir_address = kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, NtSystemRoot);

                ucs.Buffer = dir_address - obj_address;
                ucs.Length = static_cast<uint16_t>(windows_dir_size);
                ucs.MaximumLength = ucs.Length;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> sysdir_obj{c.emu, windir_obj.value() + windir_obj.size()};
            sysdir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"C:\\WINDOWS\\System32");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> base_dir_obj{c.emu, sysdir_obj.value() + sysdir_obj.size()};
            base_dir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"\\Sessions\\1\\BaseNamedObjects");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            c.emu.write_memory(obj_address + 0x9C8, 0xFFFFFFFF); // TIME_ZONE_ID_INVALID

            // Windows 2019 offset!
            c.emu.write_memory(obj_address + 0xA70, 0xFFFFFFFF); // TIME_ZONE_ID_INVALID

            if (view_size)
            {
                view_size.write(shared_section_size);
            }

            base_address.write(address);

            return STATUS_SUCCESS;
        }

        if (section_handle == DBWIN_BUFFER)
        {
            const auto dbwin_buffer_section_size = c.proc.dbwin_buffer_size;
            const auto address = c.proc.dbwin_buffer;

            if (view_size)
            {
                view_size.write(dbwin_buffer_section_size);
            }

            base_address.write(address);

            return STATUS_SUCCESS;
        }

        auto* section_entry = c.proc.sections.get(section_handle);
        if (!section_entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_entry->is_image())
        {
            const auto* binary = c.win_emu.mod_manager.map_module(section_entry->file_name, c.win_emu.log);
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
        std::vector<std::byte> file_data{};

        if (!section_entry->file_name.empty())
        {
            if (!utils::io::read_file(section_entry->file_name, &file_data))
            {
                return STATUS_INVALID_PARAMETER;
            }

            size = page_align_up(file_data.size());
        }

        const auto reserve_only = section_entry->allocation_attributes == SEC_RESERVE;
        const auto protection = map_nt_to_emulator_protection(section_entry->section_page_protection);
        const auto address = c.win_emu.memory.allocate_memory(static_cast<size_t>(size), protection, reserve_only);

        if (!reserve_only && !file_data.empty())
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

    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, const handle process_handle, const uint64_t base_address)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (!base_address)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (base_address == c.proc.shared_section_address)
        {
            c.proc.shared_section_address = 0;
            c.win_emu.memory.release_memory(base_address, static_cast<size_t>(c.proc.shared_section_size));
            return STATUS_SUCCESS;
        }

        if (base_address == c.proc.dbwin_buffer)
        {
            c.proc.dbwin_buffer = 0;
            c.win_emu.memory.release_memory(base_address, static_cast<size_t>(c.proc.dbwin_buffer_size));
            return STATUS_SUCCESS;
        }

        const auto* mod = c.win_emu.mod_manager.find_by_address(base_address);
        if (mod != nullptr)
        {
            if (c.win_emu.mod_manager.unmap(base_address))
            {
                return STATUS_SUCCESS;
            }

            return STATUS_INVALID_PARAMETER;
        }

        if (c.win_emu.memory.release_memory(base_address, 0))
        {
            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unmapping non-module/non-memory section not supported!\n");
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, const handle process_handle, const uint64_t base_address,
                                           const ULONG /*flags*/)
    {
        return handle_NtUnmapViewOfSection(c, process_handle, base_address);
    }

    NTSTATUS handle_NtAreMappedFilesTheSame()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
