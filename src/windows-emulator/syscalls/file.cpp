#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "utils/io.hpp"

#include <iostream>
#include <utils/finally.hpp>

#include <sys/stat.h>

namespace syscalls
{
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

        if (info_class == FileBasicInformation)
        {
            return STATUS_NOT_SUPPORTED;
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

    NTSTATUS handle_NtQueryVolumeInformationFile(
        const syscall_context& c, const handle file_handle,
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t fs_information,
        const ULONG length, const FS_INFORMATION_CLASS fs_information_class)
    {
        switch (fs_information_class)
        {
        case FileFsDeviceInformation:
            return handle_query<FILE_FS_DEVICE_INFORMATION>(
                c.emu, fs_information, length, io_status_block, [&](FILE_FS_DEVICE_INFORMATION& info) {
                    if (file_handle == STDOUT_HANDLE && !c.win_emu.buffer_stdout)
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

        case FileFsSizeInformation:
            return handle_query<FILE_FS_SIZE_INFORMATION>(c.emu, fs_information, length, io_status_block,
                                                          [&](FILE_FS_SIZE_INFORMATION& info) {
                                                              info.BytesPerSector = 0x1000;
                                                              info.SectorsPerAllocationUnit = 0x1000;
                                                              info.TotalAllocationUnits.QuadPart = 0x10000;
                                                              info.AvailableAllocationUnits.QuadPart = 0x1000;
                                                          });

        case FileFsVolumeInformation:
            return handle_query<FILE_FS_VOLUME_INFORMATION>(c.emu, fs_information, length, io_status_block,
                                                            [&](FILE_FS_VOLUME_INFORMATION&) {});

        default:
            c.win_emu.log.error("Unsupported fs info class: %X\n", fs_information_class);
            c.emu.stop();
            return write_io_status(io_status_block, STATUS_NOT_SUPPORTED, true);
        }
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
            f->enumeration_state->files = scan_directory(c.win_emu.file_sys.translate(f->name));
        }

        auto& enum_state = *f->enumeration_state;

        uint64_t current_offset{0};
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

                object.access([&](T& dir_info) {
                    dir_info.NextEntryOffset = static_cast<ULONG>(new_offset - object_offset); //
                });
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
        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
        block.Status = STATUS_SUCCESS;
        block.Information = 0;

        const auto _ = utils::finally([&] {
            if (io_status_block)
            {
                io_status_block.write(block);
            }
        });

        const auto ret = [&](const NTSTATUS status) {
            block.Status = status;
            return status;
        };

        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return ret(STATUS_INVALID_HANDLE);
        }

        if (info_class == FileNameInformation || info_class == FileNormalizedNameInformation)
        {
            const auto relative_path = u"\\" + windows_path(f->name).without_drive().u16string();
            const auto required_length = sizeof(FILE_NAME_INFORMATION) + (relative_path.size() * 2);

            block.Information = required_length;

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            c.emu.write_memory(file_information, FILE_NAME_INFORMATION{
                                                     .FileNameLength = static_cast<ULONG>(relative_path.size() * 2),
                                                     .FileName = {},
                                                 });

            c.emu.write_memory(file_information + offsetof(FILE_NAME_INFORMATION, FileName), relative_path.c_str(),
                               (relative_path.size() + 1) * 2);

            return ret(STATUS_SUCCESS);
        }

        if (info_class == FileStandardInformation)
        {
            block.Information = sizeof(FILE_STANDARD_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            const emulator_object<FILE_STANDARD_INFORMATION> info{c.emu, file_information};
            FILE_STANDARD_INFORMATION i{};
            i.Directory = f->is_directory() ? TRUE : FALSE;

            if (f->handle)
            {
                i.EndOfFile.QuadPart = f->handle.size();
            }

            info.write(i);

            return ret(STATUS_SUCCESS);
        }

        if (info_class == FilePositionInformation)
        {
            if (!f->handle)
            {
                return ret(STATUS_NOT_SUPPORTED);
            }

            block.Information = sizeof(FILE_POSITION_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            const emulator_object<FILE_POSITION_INFORMATION> info{c.emu, file_information};
            FILE_POSITION_INFORMATION i{};

            i.CurrentByteOffset.QuadPart = f->handle.tell();

            info.write(i);

            return ret(STATUS_SUCCESS);
        }

        if (info_class == FileAttributeTagInformation)
        {
            if (!f->handle)
            {
                return ret(STATUS_NOT_SUPPORTED);
            }

            block.Information = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            const emulator_object<FILE_ATTRIBUTE_TAG_INFORMATION> info{c.emu, file_information};
            FILE_ATTRIBUTE_TAG_INFORMATION i{};

            i.FileAttributes = f->is_directory() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

            info.write(i);

            return ret(STATUS_SUCCESS);
        }

        c.win_emu.log.error("Unsupported query file info class: %X\n", info_class);
        c.emu.stop();

        return ret(STATUS_NOT_SUPPORTED);
    }

    void commit_file_data(const std::string_view data, emulator& emu,
                          const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                          const uint64_t buffer)
    {
        if (io_status_block)
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = data.size();
            io_status_block.write(block);
        }

        emu.write_memory(buffer, data.data(), data.size());
    }

    NTSTATUS handle_NtReadFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/,
                               const uint64_t /*apc_routine*/, const uint64_t /*apc_context*/,
                               const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                               const uint64_t buffer, const ULONG length,
                               const emulator_object<LARGE_INTEGER> /*byte_offset*/,
                               const emulator_object<ULONG> /*key*/)
    {
        std::string temp_buffer{};
        temp_buffer.resize(length);

        if (file_handle == STDIN_HANDLE)
        {
            char chr{};
            if (std::cin.readsome(&chr, 1) <= 0)
            {
                std::cin.read(&chr, 1);
            }

            std::cin.putback(chr);

            const auto read_count =
                std::cin.readsome(temp_buffer.data(), static_cast<std::streamsize>(temp_buffer.size()));
            const auto count = std::max(read_count, static_cast<std::streamsize>(0));

            commit_file_data(std::string_view(temp_buffer.data(), static_cast<size_t>(count)), c.emu, io_status_block,
                             buffer);
            return STATUS_SUCCESS;
        }

        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto bytes_read = fread(temp_buffer.data(), 1, temp_buffer.size(), f->handle);
        commit_file_data(std::string_view(temp_buffer.data(), bytes_read), c.emu, io_status_block, buffer);

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

            c.win_emu.callbacks.on_stdout(temp_buffer);

            if (!temp_buffer.ends_with("\n"))
            {
                temp_buffer.push_back('\n');
            }

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

    std::optional<std::u16string_view> get_io_device_name(const std::u16string_view filename)
    {
        constexpr std::u16string_view device_prefix = u"\\Device\\";
        if (filename.starts_with(device_prefix))
        {
            return filename.substr(device_prefix.size());
        }

        constexpr std::u16string_view unc_prefix = u"\\??\\";
        if (!filename.starts_with(unc_prefix))
        {
            return std::nullopt;
        }

        const auto path = filename.substr(unc_prefix.size());

        const std::set<std::u16string, std::less<>> devices{
            u"Nsi",
            u"MountPointManager",
        };

        if (devices.contains(path))
        {
            return path;
        }

        return std::nullopt;
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
        auto filename = read_unicode_string(c.emu, attributes.ObjectName);

        auto printer = utils::finally([&] {
            c.win_emu.log.print(color::dark_gray, "--> Opening file: %s\n", u16_to_u8(filename).c_str()); //
        });

        const auto io_device_name = get_io_device_name(filename);
        if (io_device_name.has_value())
        {
            const io_device_creation_data data{
                .buffer = ea_buffer,
                .length = ea_length,
            };

            io_device_container container{std::u16string(*io_device_name), c.win_emu, data};

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

            const auto has_separator = root->name.ends_with(u"\\") || root->name.ends_with(u"/");
            f.name = root->name + (has_separator ? u"" : u"\\") + f.name;
        }

        printer.cancel();

        if (f.name.ends_with(u"\\") || create_options & FILE_DIRECTORY_FILE)
        {
            c.win_emu.log.print(color::dark_gray, "--> Opening folder: %s\n", u16_to_u8(f.name).c_str());

            if (create_disposition & FILE_CREATE)
            {
                std::error_code ec{};
                create_directory(c.win_emu.file_sys.translate(f.name), ec);

                if (ec)
                {
                    return STATUS_ACCESS_DENIED;
                }
            }
            else if (!is_directory(c.win_emu.file_sys.translate(f.name)))
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto handle = c.proc.files.store(std::move(f));
            file_handle.write(handle);

            return STATUS_SUCCESS;
        }

        c.win_emu.log.print(color::dark_gray, "--> Opening file: %s\n", u16_to_u8(f.name).c_str());

        const windows_path path = f.name;
        std::u16string mode = map_mode(desired_access, create_disposition);

        if (mode.empty() || path.is_relative())
        {
            return STATUS_NOT_SUPPORTED;
        }

        FILE* file{};

        const auto error = open_unicode(&file, c.win_emu.file_sys.translate(path), mode);

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

    NTSTATUS handle_NtQueryFullAttributesFile(
        const syscall_context& c, const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
        const emulator_object<FILE_NETWORK_OPEN_INFORMATION> file_information)
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

        c.win_emu.log.print(color::dark_gray, "--> Querying file attributes: %s\n", u16_to_u8(filename).c_str());

        const auto local_filename = c.win_emu.file_sys.translate(filename).string();

        struct _stat64 file_stat{};
        if (_stat64(local_filename.c_str(), &file_stat) != 0)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        file_information.access([&](FILE_NETWORK_OPEN_INFORMATION& info) {
            info.CreationTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastAccessTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastWriteTime = utils::convert_unix_to_windows_time(file_stat.st_mtime);
            info.AllocationSize.QuadPart = file_stat.st_size;
            info.EndOfFile.QuadPart = file_stat.st_size;
            info.ChangeTime = info.LastWriteTime;
            info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        });

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

        c.win_emu.log.print(color::dark_gray, "--> Querying file attributes: %s\n", u16_to_u8(filename).c_str());

        const auto local_filename = c.win_emu.file_sys.translate(filename).string();

        struct _stat64 file_stat{};
        if (_stat64(local_filename.c_str(), &file_stat) != 0)
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

    NTSTATUS handle_NtOpenDirectoryObject(
        const syscall_context& c, const emulator_object<handle> directory_handle, const ACCESS_MASK /*desired_access*/,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto object_name = read_unicode_string(c.emu, attributes.ObjectName);

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
        const auto object_name = read_unicode_string(c.emu, attributes.ObjectName);

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

    NTSTATUS handle_NtCreateNamedPipeFile(
        const syscall_context& c, const emulator_object<handle> /*file_handle*/, const ULONG /*desired_access*/,
        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/, const ULONG /*share_access*/,
        const ULONG /*create_disposition*/, const ULONG /*create_options*/, const ULONG /*named_pipe_type*/,
        const ULONG /*read_mode*/, const ULONG /*completion_mode*/, const ULONG /*maximum_instances*/,
        const ULONG /*inbound_quota*/, const ULONG /*outbound_quota*/,
        const emulator_object<LARGE_INTEGER> /*default_timeout*/)
    {
        c.win_emu.log.error("Unimplemented syscall NtCreateNamedPipeFile!");
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtFsControlFile(const syscall_context& c, const handle /*event_handle*/,
                                    const uint64_t /*apc_routine*/, const uint64_t /*app_context*/,
                                    const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                    const ULONG /*fs_control_code*/, const uint64_t /*input_buffer*/,
                                    const ULONG /*input_buffer_length*/, const uint64_t /*output_buffer*/,
                                    const ULONG /*output_buffer_length*/)
    {
        c.win_emu.log.error("Unimplemented syscall NtFsControlFile!");
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtFlushBuffersFile(
        const syscall_context& c, const handle file_handle,
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/)
    {
        if (file_handle == STDOUT_HANDLE)
        {
            return STATUS_SUCCESS;
        }

        const auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            return STATUS_INVALID_HANDLE;
        }

        (void)fflush(f->handle);
        return STATUS_SUCCESS;
    }
}