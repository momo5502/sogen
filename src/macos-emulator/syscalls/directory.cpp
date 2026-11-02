#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr uint32_t ATTR_CMN_NAME = 0x1;
        constexpr uint32_t ATTR_CMN_DEVID = 0x2;
        constexpr uint32_t ATTR_CMN_OBJTYPE = 0x8;
        constexpr uint32_t ATTR_CMN_FILEID = 0x2000000;
        constexpr uint32_t ATTR_CMN_RETURNED_ATTRS = 0x80000000;
        constexpr uint32_t ATTR_FILE_LINKCOUNT = 0x1;

        constexpr uint32_t SUPPORTED_COMMON_ATTRIBUTES =
            ATTR_CMN_NAME | ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID | ATTR_CMN_RETURNED_ATTRS;
        constexpr uint32_t SUPPORTED_FILE_ATTRIBUTES = ATTR_FILE_LINKCOUNT;

        constexpr uint16_t ATTR_BIT_MAP_COUNT = 5;
        constexpr uint64_t FSOPT_PACK_INVAL_ATTRS = 0x8;

        constexpr uint32_t VNON = 0;
        constexpr uint32_t VREG = 1;
        constexpr uint32_t VDIR = 2;
        constexpr uint32_t VLNK = 5;

        constexpr size_t ATTRIBUTE_ALIGNMENT = 4;
        constexpr size_t BULK_ENTRY_ALIGNMENT = 8;

        struct attribute_set
        {
            uint32_t common;
            uint32_t volume;
            uint32_t directory;
            uint32_t file;
            uint32_t fork;
        };

        struct attribute_list
        {
            uint16_t bitmap_count;
            uint16_t reserved;
            attribute_set attributes;
        };

        static_assert(sizeof(attribute_list) == 24);

        struct directory_entry
        {
            std::string name{};
            uint64_t file_id{};
            uint32_t vnode_type{};
        };

        uint32_t vnode_type(const std::filesystem::directory_entry& entry)
        {
            std::error_code ec{};
            switch (entry.symlink_status(ec).type())
            {
            case std::filesystem::file_type::regular:
                return VREG;
            case std::filesystem::file_type::directory:
                return VDIR;
            case std::filesystem::file_type::symlink:
                return VLNK;
            default:
                return VNON;
            }
        }

        directory_entry make_dot_entry(macos_emulator& emu, const posix_fd& directory, std::string name)
        {
            const auto guest_path = posix_file_system::resolve_guest_path_string(directory.guest_path, name);
            return {.name = std::move(name), .file_id = emu.file_ids.id_for(guest_path).object_id, .vnode_type = VDIR};
        }

        bool is_dot_entry(const directory_entry& entry)
        {
            return entry.name == "." || entry.name == "..";
        }

        // Entries come in readdir order, "." and ".." first; getattrlistbulk skips those two, like XNU's.
        std::vector<directory_entry> list_directory(macos_emulator& emu, const posix_fd& directory)
        {
            std::vector<directory_entry> entries{};
            std::error_code ec{};
            for (const auto& entry : std::filesystem::directory_iterator(path_from_utf8(directory.host_path), ec))
            {
                auto name = utf8_path_string(entry.path().filename());
                const auto guest_path = posix_file_system::resolve_guest_path_string(directory.guest_path, name);
                entries.push_back(
                    {.name = std::move(name), .file_id = emu.file_ids.id_for(guest_path).object_id, .vnode_type = vnode_type(entry)});
            }

            std::ranges::sort(entries, {}, &directory_entry::name);
            entries.insert(entries.begin(), {make_dot_entry(emu, directory, "."), make_dot_entry(emu, directory, "..")});
            return entries;
        }

        uint8_t dirent_type(const uint32_t type)
        {
            constexpr uint8_t dt_unknown = 0;
            constexpr uint8_t dt_dir = 4;
            constexpr uint8_t dt_reg = 8;
            constexpr uint8_t dt_lnk = 10;

            switch (type)
            {
            case VREG:
                return dt_reg;
            case VDIR:
                return dt_dir;
            case VLNK:
                return dt_lnk;
            default:
                return dt_unknown;
            }
        }

        // struct direntry (sys/dirent.h with 64-bit inodes), cut after the name as XNU's DIRENT64_LEN does.
        std::vector<std::byte> pack_dirent(const directory_entry& entry, const uint64_t next_position)
        {
            constexpr size_t name_offset = 21;

            const auto name_length = static_cast<uint16_t>(entry.name.size());
            const auto record_length = static_cast<uint16_t>((name_length + 32) & ~7);
            const auto type = dirent_type(entry.vnode_type);

            std::vector<std::byte> record(record_length);
            std::memcpy(record.data(), &entry.file_id, sizeof(entry.file_id));
            std::memcpy(record.data() + 8, &next_position, sizeof(next_position));
            std::memcpy(record.data() + 16, &record_length, sizeof(record_length));
            std::memcpy(record.data() + 18, &name_length, sizeof(name_length));
            std::memcpy(record.data() + 20, &type, sizeof(type));
            std::memcpy(record.data() + name_offset, entry.name.data(), entry.name.size());
            return record;
        }

        size_t align_up(const size_t value, const size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        // One getattrlistbulk entry: its length, then each requested attribute in bitmap order, 4-byte aligned,
        // with variable-length data (the name) after all fixed-size attributes. An attrreference's offset is relative
        // to the reference itself.
        class bulk_entry_builder
        {
          public:
            template <typename T>
            void append(const T& value)
            {
                const auto start = this->fixed_.size();
                this->fixed_.resize(align_up(start + sizeof(value), ATTRIBUTE_ALIGNMENT));
                std::memcpy(this->fixed_.data() + start, &value, sizeof(value));
            }

            void append_name(const std::string& name)
            {
                this->name_reference_ = this->fixed_.size();
                this->append(std::array<uint32_t, 2>{});
                this->name_ = name;
            }

            std::vector<std::byte> finish() const
            {
                const auto fixed_size = sizeof(uint32_t) + this->fixed_.size();
                const auto name_size = this->name_reference_ ? this->name_.size() + 1 : 0;
                const auto padded_size = fixed_size + align_up(name_size, ATTRIBUTE_ALIGNMENT);
                const auto length = static_cast<uint32_t>(align_up(padded_size, BULK_ENTRY_ALIGNMENT));

                std::vector<std::byte> entry(length);
                std::memcpy(entry.data(), &length, sizeof(length));
                std::memcpy(entry.data() + sizeof(length), this->fixed_.data(), this->fixed_.size());

                if (this->name_reference_)
                {
                    const auto reference_position = sizeof(uint32_t) + *this->name_reference_;
                    const std::array<uint32_t, 2> reference{static_cast<uint32_t>(fixed_size - reference_position),
                                                            static_cast<uint32_t>(name_size)};
                    std::memcpy(entry.data() + reference_position, reference.data(), sizeof(reference));
                    std::memcpy(entry.data() + fixed_size, this->name_.data(), this->name_.size());
                }

                return entry;
            }

          private:
            std::vector<std::byte> fixed_{};
            std::optional<size_t> name_reference_{};
            std::string name_{};
        };

        // File attributes do not apply to directories: they are left out of the returned set and, when the caller
        // asks for invalid attributes to be packed, take up their slot as zero.
        std::vector<std::byte> pack_bulk_entry(const directory_entry& entry, const attribute_set& requested, const bool pack_invalid)
        {
            const auto is_directory = entry.vnode_type == VDIR;
            const auto file_attributes = is_directory ? 0 : requested.file;

            bulk_entry_builder builder{};
            if (requested.common & ATTR_CMN_RETURNED_ATTRS)
            {
                builder.append(attribute_set{.common = requested.common, .volume = 0, .directory = 0, .file = file_attributes, .fork = 0});
            }

            if (requested.common & ATTR_CMN_NAME)
            {
                builder.append_name(entry.name);
            }

            if (requested.common & ATTR_CMN_DEVID)
            {
                builder.append(macos_file_ids::ROOT_FSID[0]);
            }

            if (requested.common & ATTR_CMN_OBJTYPE)
            {
                builder.append(entry.vnode_type);
            }

            if (requested.common & ATTR_CMN_FILEID)
            {
                builder.append(entry.file_id);
            }

            if ((requested.file & ATTR_FILE_LINKCOUNT) && (!is_directory || pack_invalid))
            {
                builder.append(uint32_t{is_directory ? 0U : 1U});
            }

            return builder.finish();
        }

        // getattrlistbulk(2) requires the name and the returned attribute set in every entry.
        bool is_valid_request(const attribute_list& request)
        {
            constexpr uint32_t required_common_attributes = ATTR_CMN_NAME | ATTR_CMN_RETURNED_ATTRS;
            return request.bitmap_count == ATTR_BIT_MAP_COUNT &&
                   (request.attributes.common & required_common_attributes) == required_common_attributes;
        }

        bool is_supported_request(const attribute_set& requested)
        {
            return (requested.common & ~SUPPORTED_COMMON_ATTRIBUTES) == 0 && requested.volume == 0 && requested.directory == 0 &&
                   (requested.file & ~SUPPORTED_FILE_ATTRIBUTES) == 0 && requested.fork == 0;
        }
    }

    void sys_fchdir(const macos_syscall_context& c)
    {
        const auto* directory = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (directory == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (directory->type != fd_type::directory)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTDIR);
            return;
        }

        c.emu_ref.working_directory = directory->guest_path;
        write_macos_syscall_success(c, 0);
    }

    void sys_chdir(const macos_syscall_context& c)
    {
        const auto requested_path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        if (!requested_path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        const auto guest_path = c.emu_ref.resolve_guest_path(*requested_path);
        const auto host_path = c.emu_ref.file_system.translate(guest_path);
        if (requested_path->empty() || !std::filesystem::exists(host_path))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        if (!std::filesystem::is_directory(host_path))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTDIR);
            return;
        }

        c.emu_ref.working_directory = guest_path;
        write_macos_syscall_success(c, 0);
    }

    void sys_getattrlistbulk(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 2);
        const auto buffer_size = get_macos_syscall_argument(c.emu, 3);
        const auto options = get_macos_syscall_argument(c.emu, 4);

        attribute_list request{};
        if (!c.emu.try_read_memory(get_macos_syscall_argument(c.emu, 1), &request, sizeof(request)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        auto* directory = c.emu_ref.fds.get(fd);
        if (directory == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (directory->type != fd_type::directory)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOTDIR);
            return;
        }

        if (!is_valid_request(request))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto& requested = request.attributes;
        if (!is_supported_request(requested))
        {
            c.emu_ref.log.error("Unhandled getattrlistbulk(common 0x%x, dir 0x%x, file 0x%x)\n", requested.common, requested.directory,
                                requested.file);
            c.emu_ref.stop();
            return;
        }

        const auto pack_invalid = (options & FSOPT_PACK_INVAL_ATTRS) != 0;
        const auto entries = list_directory(c.emu_ref, *directory);

        std::vector<std::byte> packed{};
        uint64_t returned = 0;
        auto position = directory->directory_position;
        for (; position < entries.size(); ++position)
        {
            const auto& entry = entries.at(static_cast<size_t>(position));
            if (is_dot_entry(entry))
            {
                continue;
            }

            const auto record = pack_bulk_entry(entry, requested, pack_invalid);
            if (packed.size() + record.size() > buffer_size)
            {
                break;
            }

            packed.insert(packed.end(), record.begin(), record.end());
            ++returned;
        }

        if (returned == 0 && position < entries.size())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ERANGE);
            return;
        }

        if (!c.emu.try_write_memory(buffer, packed.data(), packed.size()))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        c.emu_ref.log.log("getattrlistbulk(%d, %s) -> %llu entries\n", fd, directory->guest_path.c_str(),
                          static_cast<unsigned long long>(returned));
        directory->directory_position = position;
        write_macos_syscall_success(c, returned);
    }

    // Buffers of GETDIRENTRIES64_EXTENDED_BUFSIZE bytes or more end with flags that tell libc it reached the end
    // of the directory, which saves it the call that would return nothing.
    void sys_getdirentries64(const macos_syscall_context& c)
    {
        constexpr uint64_t extended_buffer_size = 1024;
        constexpr uint32_t end_of_directory = 0x1;

        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 0));
        const auto buffer = get_macos_syscall_argument(c.emu, 1);
        const auto buffer_size = get_macos_syscall_argument(c.emu, 2);
        const auto position_pointer = get_macos_syscall_argument(c.emu, 3);

        auto* directory = c.emu_ref.fds.get(fd);
        if (directory == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        if (directory->type != fd_type::directory)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto is_extended = buffer_size >= extended_buffer_size;
        const auto entries_size = is_extended ? buffer_size - sizeof(end_of_directory) : buffer_size;
        const auto entries = list_directory(c.emu_ref, *directory);

        std::vector<std::byte> packed{};
        auto position = directory->directory_position;
        for (; position < entries.size(); ++position)
        {
            const auto record = pack_dirent(entries.at(static_cast<size_t>(position)), position + 1);
            if (packed.size() + record.size() > entries_size)
            {
                break;
            }

            packed.insert(packed.end(), record.begin(), record.end());
        }

        if (packed.empty() && position < entries.size())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto start_position = directory->directory_position;
        const auto flags = position == entries.size() ? end_of_directory : 0;
        if (!c.emu.try_write_memory(buffer, packed.data(), packed.size()) ||
            !c.emu.try_write_memory(position_pointer, &start_position, sizeof(start_position)) ||
            (is_extended && !c.emu.try_write_memory(buffer + buffer_size - sizeof(flags), &flags, sizeof(flags))))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        c.emu_ref.log.log("getdirentries64(%d, %s) -> %llu entries\n", fd, directory->guest_path.c_str(),
                          static_cast<unsigned long long>(position - start_position));
        directory->directory_position = position;
        write_macos_syscall_success(c, packed.size());
    }
}
