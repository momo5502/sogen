#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "utils/io.hpp"

#include <iostream>
#include <utils/finally.hpp>
#include <utils/wildcard.hpp>

#include <sys/stat.h>

#include "../devices/named_pipe.hpp"

#if defined(OS_WINDOWS)
#define fstat64 _fstat64
#elif defined(OS_MAC)
#define fstat64 fstat
#endif

namespace syscalls
{
    namespace
    {
        std::pair<utils::file_handle, NTSTATUS> open_file(const file_system& file_sys, const windows_path& path, const std::u16string& mode)
        {
            FILE* file{};
            const auto error = open_unicode(&file, file_sys.translate(path), mode);

            if (file)
            {
                return {file, STATUS_SUCCESS};
            }

            using fh = utils::file_handle;

            switch (error)
            {
            case ENOENT:
                return {fh{}, STATUS_OBJECT_NAME_NOT_FOUND};
            case EACCES:
                return {fh{}, STATUS_ACCESS_DENIED};
            case EISDIR:
                return {fh{}, STATUS_FILE_IS_A_DIRECTORY};
            default:
                return {fh{}, STATUS_NOT_SUPPORTED};
            }
        }
    }

    NTSTATUS handle_NtSetInformationFile(const syscall_context& c, const handle file_handle,
                                         const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                         const uint64_t file_information, const ULONG length, const FILE_INFORMATION_CLASS info_class)
    {
        auto* f = c.proc.files.get(file_handle);
        if (!f)
        {
            if (c.proc.devices.get(file_handle))
            {
                c.win_emu.log.error("Unsupported set device info class: %X\n", info_class);
                return STATUS_SUCCESS;
            }

            return STATUS_INVALID_HANDLE;
        }

        if (info_class == FileRenameInformation)
        {
            if (length < sizeof(FILE_RENAME_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto info = c.emu.read_memory<FILE_RENAME_INFORMATION>(file_information);
            auto new_name =
                read_string<char16_t>(c.emu, file_information + offsetof(FILE_RENAME_INFORMATION, FileName), info.FileNameLength / 2);

            if (info.RootDirectory)
            {
                const auto* root = c.proc.files.get(info.RootDirectory);
                if (!root)
                {
                    return STATUS_INVALID_HANDLE;
                }

                const auto has_separator = root->name.ends_with(u"\\") || root->name.ends_with(u"/");
                new_name = root->name + (has_separator ? u"" : u"\\") + new_name;
            }

            c.win_emu.log.warn("--> File rename requested: %s --> %s\n", u16_to_u8(f->name).c_str(), u16_to_u8(new_name).c_str());

            std::error_code ec{};
            bool file_exists = std::filesystem::exists(new_name, ec);

            if (ec)
            {
                return STATUS_ACCESS_DENIED;
            }

            if (!info.ReplaceIfExists && file_exists)
            {
                return STATUS_OBJECT_NAME_EXISTS;
            }

            f->handle.defer_rename(c.win_emu.file_sys.translate(f->name), c.win_emu.file_sys.translate(new_name));

            return STATUS_SUCCESS;
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

    NTSTATUS handle_NtQueryVolumeInformationFile(const syscall_context& c, const handle file_handle,
                                                 const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                                 const uint64_t fs_information, const ULONG length,
                                                 const FS_INFORMATION_CLASS fs_information_class)
    {
        switch (fs_information_class)
        {
        case FileFsDeviceInformation:
            return handle_query<FILE_FS_DEVICE_INFORMATION>(c.emu, fs_information, length, io_status_block,
                                                            [&](FILE_FS_DEVICE_INFORMATION& info) {
                                                                if (file_handle == STDOUT_HANDLE)
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

        case FileFsAttributeInformation:
            return handle_query<_FILE_FS_ATTRIBUTE_INFORMATION>(
                c.emu, fs_information, length, io_status_block, [&](_FILE_FS_ATTRIBUTE_INFORMATION& info) {
                    info.FileSystemAttributes = 0x40006; // FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_NAMED_STREAMS
                    info.MaximumComponentNameLength = 255;
                    constexpr auto name = u"NTFS"sv;
                    info.FileSystemNameLength = static_cast<ULONG>(name.size() * sizeof(char16_t));
                    memcpy(info.FileSystemName, name.data(), info.FileSystemNameLength);
                });

        default:
            c.win_emu.log.error("Unsupported fs info class: 0x%X\n", fs_information_class);
            c.emu.stop();
            return write_io_status(io_status_block, STATUS_NOT_SUPPORTED, true);
        }
    }

    std::vector<file_entry> scan_directory(const file_system& file_sys, const windows_path& win_path, const std::u16string_view file_mask)
    {
        std::vector<file_entry> files{};

        const auto dir = file_sys.translate(win_path);

        if (file_mask.empty() || file_mask == u"*")
        {
            files.emplace_back(file_entry{.file_path = ".", .is_directory = true});
            files.emplace_back(file_entry{.file_path = "..", .is_directory = true});
        }

        std::error_code ec{};
        for (const auto& file : std::filesystem::directory_iterator(dir, ec))
        {
            if (!file_mask.empty() && !utils::wildcard::match_filename(file.path().filename().u16string(), file_mask))
            {
                continue;
            }

            files.emplace_back(file_entry{
                .file_path = file.path().filename(),
                .file_size = file.is_directory() ? 0 : file.file_size(),
                .is_directory = file.is_directory(),
            });
        }

        file_sys.access_mapped_entries(win_path, [&](const std::pair<windows_path, std::filesystem::path>& entry) {
            const auto filename = entry.first.leaf();

            if (!file_mask.empty() && !utils::wildcard::match_filename(filename, file_mask))
            {
                return;
            }

            const std::filesystem::directory_entry dir_entry(entry.second, ec);
            if (ec || !dir_entry.exists())
            {
                return;
            }

            files.emplace_back(file_entry{
                .file_path = filename,
                .file_size = dir_entry.is_directory() ? 0 : dir_entry.file_size(),
                .is_directory = dir_entry.is_directory(),
            });
        });

        return files;
    }

    template <typename T>
    NTSTATUS handle_file_enumeration(const syscall_context& c,
                                     const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                     const uint64_t file_information, const uint32_t length, const ULONG query_flags,
                                     const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> file_mask, file* f)
    {
        if (!f->enumeration_state || query_flags & SL_RESTART_SCAN)
        {
            const auto mask = file_mask ? read_unicode_string(c.emu, file_mask) : u"";
            c.win_emu.callbacks.on_generic_access("Enumerating directory", f->name);

            f->enumeration_state.emplace(file_enumeration_state{});
            f->enumeration_state->files = scan_directory(c.win_emu.file_sys, f->name, mask);
        }

        auto& enum_state = *f->enumeration_state;

        uint64_t current_offset{0};
        emulator_object<T> object{c.emu};

        size_t current_index = enum_state.current_index;

        if (current_index >= enum_state.files.size())
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = 0;
            io_status_block.write(block);

            return STATUS_NO_MORE_FILES;
        }

        do
        {
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
            info.FileAttributes = current_file.is_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            info.FileNameLength = static_cast<ULONG>(file_name.size() * 2);
            info.EndOfFile.QuadPart = current_file.file_size;

            object.set_address(file_information + new_offset);
            object.write(info);

            c.emu.write_memory(object.value() + offsetof(T, FileName), file_name.data(), info.FileNameLength);

            ++current_index;
            current_offset = end_offset;
        } while ((query_flags & SL_RETURN_SINGLE_ENTRY) == 0 && current_index < enum_state.files.size());

        if ((query_flags & SL_NO_CURSOR_UPDATE) == 0)
        {
            enum_state.current_index = current_index;
        }

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
        block.Information = current_offset;
        io_status_block.write(block);

        return current_index <= enum_state.files.size() ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
    }

    NTSTATUS handle_NtQueryDirectoryFileEx(const syscall_context& c, const handle file_handle, const handle /*event_handle*/,
                                           const EMULATOR_CAST(emulator_pointer, PIO_APC_ROUTINE) /*apc_routine*/,
                                           const emulator_pointer /*apc_context*/,
                                           const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                           const uint64_t file_information, const uint32_t length, const uint32_t info_class,
                                           const ULONG query_flags, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> file_name)
    {
        auto* f = c.proc.files.get(file_handle);
        if (!f || !f->is_directory())
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == FileDirectoryInformation)
        {
            return handle_file_enumeration<FILE_DIRECTORY_INFORMATION>(c, io_status_block, file_information, length, query_flags, file_name,
                                                                       f);
        }

        if (info_class == FileFullDirectoryInformation)
        {
            return handle_file_enumeration<FILE_FULL_DIR_INFORMATION>(c, io_status_block, file_information, length, query_flags, file_name,
                                                                      f);
        }

        if (info_class == FileBothDirectoryInformation)
        {
            return handle_file_enumeration<FILE_BOTH_DIR_INFORMATION>(c, io_status_block, file_information, length, query_flags, file_name,
                                                                      f);
        }

        c.win_emu.log.error("Unsupported query directory file info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryDirectoryFile(const syscall_context& c, const handle file_handle, const handle event_handle,
                                         const EMULATOR_CAST(emulator_pointer, PIO_APC_ROUTINE) apc_routine,
                                         const emulator_pointer apc_context,
                                         const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                         const uint64_t file_information, const uint32_t length, const uint32_t info_class,
                                         const BOOLEAN return_single_entry,
                                         const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> file_name, const BOOLEAN restart_scan)
    {
        ULONG query_flags = 0;
        if (return_single_entry)
        {
            query_flags |= SL_RETURN_SINGLE_ENTRY;
        }
        if (restart_scan)
        {
            query_flags |= SL_RESTART_SCAN;
        }
        return handle_NtQueryDirectoryFileEx(c, file_handle, event_handle, apc_routine, apc_context, io_status_block, file_information,
                                             length, info_class, query_flags, file_name);
    }

    NTSTATUS handle_NtQueryInformationFile(const syscall_context& c, const handle file_handle,
                                           const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                           const uint64_t file_information, const uint32_t length, const uint32_t info_class)
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

        if (info_class == FileBasicInformation)
        {
            block.Information = sizeof(FILE_BASIC_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            struct _stat64 file_stat{};
            if (fstat64(f->handle, &file_stat) != 0)
            {
                return STATUS_INVALID_HANDLE;
            }

            const emulator_object<FILE_BASIC_INFORMATION> info{c.emu, file_information};
            FILE_BASIC_INFORMATION i{};

            i.CreationTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            i.LastAccessTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            i.LastWriteTime = utils::convert_unix_to_windows_time(file_stat.st_mtime);
            i.ChangeTime = i.LastWriteTime;
            i.FileAttributes = (file_stat.st_mode & S_IFDIR) != 0 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

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

        if (info_class == FileIsRemoteDeviceInformation)
        {
            if (!f->handle)
            {
                return ret(STATUS_NOT_SUPPORTED);
            }

            block.Information = sizeof(FILE_IS_REMOTE_DEVICE_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            const emulator_object<FILE_IS_REMOTE_DEVICE_INFORMATION> info{c.emu, file_information};
            FILE_IS_REMOTE_DEVICE_INFORMATION i{};

            i.IsRemote = FALSE;

            info.write(i);

            return ret(STATUS_SUCCESS);
        }

        if (info_class == FileIdInformation)
        {
            if (!f->handle)
            {
                return ret(STATUS_NOT_SUPPORTED);
            }

            block.Information = sizeof(FILE_ID_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            struct _stat64 file_stat{};
            if (fstat64(f->handle, &file_stat) != 0)
            {
                return ret(STATUS_INVALID_HANDLE);
            }

            const emulator_object<FILE_ID_INFORMATION> info{c.emu, file_information};
            FILE_ID_INFORMATION i{};

            i.VolumeSerialNumber = file_stat.st_dev;
            memset(&i.FileId, 0, sizeof(i.FileId));
            memcpy(&i.FileId.Identifier[0], &file_stat.st_ino, sizeof(file_stat.st_ino));

            info.write(i);

            return ret(STATUS_SUCCESS);
        }

        if (info_class == FileAllInformation)
        {
            return ret(STATUS_NOT_SUPPORTED);
        }

        c.win_emu.log.error("Unsupported query file info class: 0x%X\n", info_class);
        c.emu.stop();

        return ret(STATUS_NOT_SUPPORTED);
    }

    NTSTATUS handle_NtQueryInformationByName(const syscall_context& c,
                                             const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                             const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block,
                                             const uint64_t file_information, const uint32_t length, const uint32_t info_class)
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

        const auto attributes = object_attributes.read();
        auto filename = read_unicode_string(c.emu, attributes.ObjectName);

        c.win_emu.callbacks.on_generic_access("Query file info", filename);

        const auto ret = [&](const NTSTATUS status) {
            block.Status = status;
            return status;
        };

        if (info_class == FileStatBasicInformation)
        {
            block.Information = sizeof(EMU_FILE_STAT_BASIC_INFORMATION);

            if (length < block.Information)
            {
                return ret(STATUS_BUFFER_OVERFLOW);
            }

            auto [native_file_handle, status] = open_file(c.win_emu.file_sys, filename, u"r");
            if (status != STATUS_SUCCESS)
            {
                return ret(status);
            }

            struct _stat64 file_stat{};
            if (fstat64(native_file_handle, &file_stat) != 0)
            {
                return STATUS_INVALID_HANDLE;
            }

            EMU_FILE_STAT_BASIC_INFORMATION i{};

            i.CreationTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            i.LastAccessTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            i.LastWriteTime = utils::convert_unix_to_windows_time(file_stat.st_mtime);
            i.ChangeTime = i.LastWriteTime;
            i.FileAttributes = (file_stat.st_mode & S_IFDIR) != 0 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

            c.emu.write_memory(file_information, i);

            return ret(STATUS_SUCCESS);
        }

        c.win_emu.log.error("Unsupported query name info class: %X\n", info_class);
        c.emu.stop();

        return ret(STATUS_NOT_SUPPORTED);
    }

    void commit_file_data(const std::string_view data, emulator& emu,
                          const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t buffer)
    {
        if (io_status_block)
        {
            IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
            block.Information = data.size();
            io_status_block.write(block);
        }

        emu.write_memory(buffer, data.data(), data.size());
    }

    NTSTATUS handle_NtReadFile(const syscall_context& c, const handle file_handle, const uint64_t /*event*/, const uint64_t /*apc_routine*/,
                               const uint64_t /*apc_context*/,
                               const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t buffer,
                               const ULONG length, const emulator_object<LARGE_INTEGER> /*byte_offset*/,
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

            const auto read_count = std::cin.readsome(temp_buffer.data(), static_cast<std::streamsize>(temp_buffer.size()));
            const auto count = std::max(read_count, static_cast<std::streamsize>(0));

            commit_file_data(std::string_view(temp_buffer.data(), static_cast<size_t>(count)), c.emu, io_status_block, buffer);
            return STATUS_SUCCESS;
        }

        const auto* container = c.proc.devices.get(file_handle);
        if (container)
        {
            if (auto* pipe = container->get_internal_device<named_pipe>())
            {
                if (!pipe->write_queue.empty())
                {
                    std::string_view data = pipe->write_queue.front();
                    const size_t to_copy = std::min<size_t>(data.size(), length);

                    commit_file_data(data.substr(0, to_copy), c.emu, io_status_block, buffer);

                    if (to_copy == data.size())
                    {
                        pipe->write_queue.pop_front();
                    }
                    else
                    {
                        pipe->write_queue.front().erase(0, to_copy);
                    }

                    return STATUS_SUCCESS;
                }

                return STATUS_PIPE_EMPTY;
            }
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
                                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const uint64_t buffer,
                                const ULONG length, const emulator_object<LARGE_INTEGER> /*byte_offset*/,
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

            return STATUS_SUCCESS;
        }

        const auto* container = c.proc.devices.get(file_handle);
        if (container)
        {
            if (auto* pipe = container->get_internal_device<named_pipe>())
            {
                (void)pipe; // For future use: suppressing compiler issues
                // TODO c.win_emu.callbacks.on_named_pipe_write(pipe->name, temp_buffer);

                // TODO pipe->write_queue.push_back(temp_buffer);

                if (io_status_block)
                {
                    IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                    block.Information = static_cast<ULONG>(temp_buffer.size());
                    io_status_block.write(block);
                }

                return STATUS_SUCCESS;
            }
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

    NTSTATUS handle_named_pipe_create(const syscall_context& c, const emulator_object<handle>& out_handle,
                                      const std::u16string_view filename, const OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>& attributes,
                                      ACCESS_MASK desired_access)
    {
        (void)attributes; // This isn't being consumed atm, suppressing errors

        c.win_emu.callbacks.on_generic_access("Creating/opening named pipe", filename);

        io_device_creation_data data{};

        std::u16string device_name = u"NamedPipe";

        io_device_container container{device_name, c.win_emu, data};

        if (auto* pipe_device = container.get_internal_device<named_pipe>())
        {
            pipe_device->name = std::u16string(filename);
            pipe_device->access = desired_access;
        }

        const auto handle = c.proc.devices.store(std::move(container));
        out_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateFile(const syscall_context& c, const emulator_object<handle> file_handle, ACCESS_MASK desired_access,
                                 const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                 const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                 const emulator_object<LARGE_INTEGER> /*allocation_size*/, ULONG /*file_attributes*/,
                                 ULONG /*share_access*/, ULONG create_disposition, ULONG create_options, uint64_t ea_buffer,
                                 ULONG ea_length)
    {
        const auto attributes = object_attributes.read();
        auto filename = read_unicode_string(c.emu, attributes.ObjectName);

        // Check for console device paths
        // Convert to uppercase for case-insensitive comparison
        std::u16string filename_upper = filename;
        std::transform(filename_upper.begin(), filename_upper.end(), filename_upper.begin(), ::towupper);

        // Handle console output device
        if (filename_upper == u"\\??\\CONOUT$" || filename_upper == u"\\DEVICE\\CONOUT$" || filename_upper == u"CONOUT$" ||
            filename_upper == u"\\??\\CON" || filename_upper == u"\\DEVICE\\CONSOLE" || filename_upper == u"CON")
        {
            c.win_emu.callbacks.on_generic_access("Opening console output", filename);
            file_handle.write(STDOUT_HANDLE);
            return STATUS_SUCCESS;
        }

        // Handle console input device
        if (filename_upper == u"\\??\\CONIN$" || filename_upper == u"\\DEVICE\\CONIN$" || filename_upper == u"CONIN$")
        {
            c.win_emu.callbacks.on_generic_access("Opening console input", filename);
            file_handle.write(STDIN_HANDLE);
            return STATUS_SUCCESS;
        }

        if (is_named_pipe_path(filename))
        {
            return handle_named_pipe_create(c, file_handle, filename, attributes, desired_access);
        }

        auto printer = utils::finally([&] {
            c.win_emu.callbacks.on_generic_access("Opening file", filename); //
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

        if (filename == u"\\??\\CONOUT$")
        {
            file_handle.write(STDOUT_HANDLE);
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

        std::error_code ec{};

        const windows_path path = f.name;
        const bool is_directory = std::filesystem::is_directory(c.win_emu.file_sys.translate(path), ec);

        if (is_directory || create_options & FILE_DIRECTORY_FILE)
        {
            c.win_emu.callbacks.on_generic_access("Opening folder", f.name);

            if (create_disposition & FILE_CREATE)
            {
                create_directory(c.win_emu.file_sys.translate(path), ec);

                if (ec)
                {
                    return STATUS_ACCESS_DENIED;
                }
            }
            else if (!is_directory)
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            const auto handle = c.proc.files.store(std::move(f));
            file_handle.write(handle);

            return STATUS_SUCCESS;
        }

        c.win_emu.callbacks.on_generic_access("Opening file", f.name);

        std::u16string mode = map_mode(desired_access, create_disposition);

        if (mode.empty() || path.is_relative())
        {
            return STATUS_NOT_SUPPORTED;
        }

        auto [native_file_handle, status] = open_file(c.win_emu.file_sys, path, mode);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }

        f.handle = std::move(native_file_handle);

        const auto handle = c.proc.files.store(std::move(f));
        file_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryFullAttributesFile(const syscall_context& c,
                                              const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
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

        auto filename = read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});

        if (attributes.RootDirectory)
        {
            const auto* root = c.proc.files.get(attributes.RootDirectory);
            if (!root)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto has_separator = root->name.ends_with(u"\\") || root->name.ends_with(u"/");
            filename = root->name + (has_separator ? u"" : u"\\") + filename;
        }

        c.win_emu.callbacks.on_generic_access("Querying file attributes", filename);

        const auto local_filename = c.win_emu.file_sys.translate(filename).u8string();

        struct _stat64 file_stat{};
        if (_stat64(reinterpret_cast<const char*>(local_filename.c_str()), &file_stat) != 0)
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
            info.FileAttributes = (file_stat.st_mode & S_IFDIR) != 0 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryAttributesFile(const syscall_context& c,
                                          const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
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

        auto filename = read_unicode_string(c.emu, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, attributes.ObjectName});

        if (attributes.RootDirectory)
        {
            const auto* root = c.proc.files.get(attributes.RootDirectory);
            if (!root)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto has_separator = root->name.ends_with(u"\\") || root->name.ends_with(u"/");
            filename = root->name + (has_separator ? u"" : u"\\") + filename;
        }

        c.win_emu.callbacks.on_generic_access("Querying file attributes", filename);

        windows_path filepath(filename);
        if (filepath.is_relative())
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        const auto local_filename = c.win_emu.file_sys.translate(filepath).u8string();

        struct _stat64 file_stat{};
        if (_stat64(reinterpret_cast<const char*>(local_filename.c_str()), &file_stat) != 0)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        file_information.access([&](FILE_BASIC_INFORMATION& info) {
            info.CreationTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastAccessTime = utils::convert_unix_to_windows_time(file_stat.st_atime);
            info.LastWriteTime = utils::convert_unix_to_windows_time(file_stat.st_mtime);
            info.ChangeTime = info.LastWriteTime;
            info.FileAttributes = (file_stat.st_mode & S_IFDIR) != 0 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenFile(const syscall_context& c, const emulator_object<handle> file_handle, const ACCESS_MASK desired_access,
                               const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                               const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, const ULONG share_access,
                               const ULONG open_options)
    {
        return handle_NtCreateFile(c, file_handle, desired_access, object_attributes, io_status_block, {c.emu}, 0, share_access, FILE_OPEN,
                                   open_options, 0, 0);
    }

    NTSTATUS handle_NtOpenDirectoryObject(const syscall_context& c, const emulator_object<handle> directory_handle,
                                          const ACCESS_MASK /*desired_access*/,
                                          const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto object_name = read_unicode_string(c.emu, attributes.ObjectName);

        if (object_name == u"\\KnownDlls")
        {
            directory_handle.write(KNOWN_DLLS_DIRECTORY);
            return STATUS_SUCCESS;
        }

        if (object_name == u"\\KnownDlls32")
        {
            directory_handle.write(KNOWN_DLLS32_DIRECTORY);
            return STATUS_SUCCESS;
        }

        if (object_name == u"\\Sessions\\1\\BaseNamedObjects")
        {
            directory_handle.write(BASE_NAMED_OBJECTS_DIRECTORY);
            return STATUS_SUCCESS;
        }

        if (object_name == u"\\RPC Control")
        {
            directory_handle.write(RPC_CONTROL_DIRECTORY);
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenSymbolicLinkObject(const syscall_context& c, const emulator_object<handle> link_handle,
                                             ACCESS_MASK /*desired_access*/,
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

        if (link_handle == KNOWN_DLLS32_SYMLINK)
        {
            constexpr std::u16string_view syswow64 = u"C:\\WINDOWS\\SysWOW64";
            constexpr auto str_length = syswow64.size() * 2;
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
                c.emu.write_memory(str.Buffer, syswow64.data(), max_length);
            });

            return too_small ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtCreateNamedPipeFile(const syscall_context& c, emulator_object<handle> file_handle, ULONG desired_access,
                                          emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                          emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block, ULONG share_access,
                                          ULONG create_disposition, ULONG create_options, ULONG named_pipe_type, ULONG read_mode,
                                          ULONG completion_mode, ULONG maximum_instances, ULONG inbound_quota, ULONG outbound_quota,
                                          emulator_object<LARGE_INTEGER> default_timeout)
    {
        (void)desired_access;
        (void)share_access;
        (void)create_disposition;
        (void)create_options;

        const auto attributes = object_attributes.read();
        const auto filename = read_unicode_string(c.emu, attributes.ObjectName);

        if (!filename.starts_with(u"\\Device\\NamedPipe"))
        {
            return STATUS_NOT_SUPPORTED;
        }

        c.win_emu.callbacks.on_generic_access("Creating named pipe", filename);

        io_device_creation_data data{};
        io_device_container container{u"NamedPipe", c.win_emu, data};

        if (auto* pipe_device = container.get_internal_device<named_pipe>())
        {
            pipe_device->name = filename;
            pipe_device->pipe_type = named_pipe_type;
            pipe_device->read_mode = read_mode;
            pipe_device->completion_mode = completion_mode;
            pipe_device->max_instances = maximum_instances;
            pipe_device->inbound_quota = inbound_quota;
            pipe_device->outbound_quota = outbound_quota;
            pipe_device->default_timeout = default_timeout.read();
        }
        else
        {
            return STATUS_NOT_SUPPORTED;
        }

        handle pipe_handle = c.proc.devices.store(std::move(container));
        file_handle.write(pipe_handle);

        IO_STATUS_BLOCK<EmulatorTraits<Emu64>> iosb{};
        iosb.Status = STATUS_SUCCESS;
        iosb.Information = 0;
        io_status_block.write(iosb);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtFsControlFile(const syscall_context& c, const handle /*event_handle*/, const uint64_t /*apc_routine*/,
                                    const uint64_t /*app_context*/,
                                    const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> /*io_status_block*/,
                                    const ULONG /*fs_control_code*/, const uint64_t /*input_buffer*/, const ULONG /*input_buffer_length*/,
                                    const uint64_t /*output_buffer*/, const ULONG /*output_buffer_length*/)
    {
        c.win_emu.log.error("Unimplemented syscall NtFsControlFile!");
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtFlushBuffersFile(const syscall_context& c, const handle file_handle,
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
