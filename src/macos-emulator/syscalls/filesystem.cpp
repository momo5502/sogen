#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    namespace
    {
        struct macos_statfs64
        {
            uint32_t f_bsize;
            int32_t f_iosize;
            uint64_t f_blocks;
            uint64_t f_bfree;
            uint64_t f_bavail;
            uint64_t f_files;
            uint64_t f_ffree;
            std::array<int32_t, 2> f_fsid;
            uint32_t f_owner;
            uint32_t f_type;
            uint32_t f_flags;
            uint32_t f_fssubtype;
            std::array<char, 16> f_fstypename;
            std::array<char, 1024> f_mntonname;
            std::array<char, 1024> f_mntfromname;
            uint32_t f_flags_ext;
            std::array<uint32_t, 7> f_reserved;
        };

        static_assert(sizeof(macos_statfs64) == 2168);
        static_assert(offsetof(macos_statfs64, f_fsid) == 48);
        static_assert(offsetof(macos_statfs64, f_mntonname) == 88);
        static_assert(offsetof(macos_statfs64, f_flags_ext) == 2136);

        constexpr uint32_t MNT_RDONLY = 0x1;
        constexpr uint32_t MNT_LOCAL = 0x1000;
        constexpr uint32_t MNT_ROOTFS = 0x4000;
        constexpr uint32_t APFS_FILESYSTEM_TYPE = 0x1A;

        template <size_t Size>
        void copy_name(std::array<char, Size>& target, const std::string_view name)
        {
            std::copy_n(name.begin(), std::min(name.size(), Size - 1), target.begin());
        }

        // The emulation root is presented as a single read-only APFS root volume carrying the synthetic fsid that
        // stat and the shared cache FileIdTuple use.
        macos_statfs64 root_filesystem()
        {
            macos_statfs64 info{};
            info.f_bsize = 4096;
            info.f_iosize = 1024 * 1024;
            info.f_blocks = 1ULL << 26;
            info.f_files = 1ULL << 22;
            info.f_fsid = macos_file_ids::ROOT_FSID;
            info.f_type = APFS_FILESYSTEM_TYPE;
            info.f_flags = MNT_RDONLY | MNT_LOCAL | MNT_ROOTFS;
            copy_name(info.f_fstypename, "apfs");
            copy_name(info.f_mntonname, "/");
            copy_name(info.f_mntfromname, "/dev/disk1s1");
            return info;
        }
    }

    namespace
    {
        // The full path comes back as an attrreference (offset from the reference, length including the NUL) after
        // the leading length, followed by the string padded to four bytes.
        void write_full_path_attribute(const macos_syscall_context& c, const std::string& guest_path)
        {
            if (!std::filesystem::exists(c.emu_ref.file_system.translate(guest_path)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
                return;
            }

            constexpr uint32_t reference_size = 2 * sizeof(uint32_t);
            const auto path_length = static_cast<uint32_t>(guest_path.size() + 1);
            const auto total_length = static_cast<uint32_t>(sizeof(uint32_t) + reference_size + ((path_length + 3) & ~3U));

            std::vector<std::byte> data(total_length);
            std::memcpy(data.data(), &total_length, sizeof(total_length));
            std::memcpy(data.data() + sizeof(uint32_t), &reference_size, sizeof(reference_size));
            std::memcpy(data.data() + sizeof(uint32_t) + sizeof(int32_t), &path_length, sizeof(path_length));
            std::memcpy(data.data() + sizeof(uint32_t) + reference_size, guest_path.c_str(), path_length);

            const auto buffer = get_macos_syscall_argument(c.emu, 2);
            const auto buffer_size = get_macos_syscall_argument(c.emu, 3);
            const auto written = std::min<uint64_t>(buffer_size, data.size());
            if (!c.emu.try_write_memory(buffer, data.data(), static_cast<size_t>(written)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }
    }

    namespace
    {
        constexpr uint32_t ATTR_CMN_DEVID = 0x2;
        constexpr uint32_t ATTR_CMN_FSID = 0x4;
        constexpr uint32_t ATTR_CMN_FULLPATH = 0x08000000;
        constexpr uint32_t ATTR_VOL_MOUNTFLAGS = 0x4000;
        constexpr uint32_t ATTR_VOL_CAPABILITIES = 0x20000;
        constexpr uint32_t ATTR_VOL_UUID = 0x40000;
        constexpr uint32_t ATTR_VOL_INFO = 0x80000000;

        constexpr uint32_t SUPPORTED_COMMON_ATTRIBUTES = ATTR_CMN_DEVID | ATTR_CMN_FSID;
        constexpr uint32_t SUPPORTED_VOLUME_ATTRIBUTES = ATTR_VOL_MOUNTFLAGS | ATTR_VOL_CAPABILITIES | ATTR_VOL_UUID | ATTR_VOL_INFO;

        constexpr uint32_t VOL_CAP_FMT_PERSISTENTOBJECTIDS = 0x1;
        constexpr std::array<uint8_t, 16> ROOT_VOLUME_UUID{0x53, 0x4F, 0x47, 0x45, 0x4E, 0x2D, 0x52, 0x4F,
                                                           0x4F, 0x54, 0x2D, 0x56, 0x4F, 0x4C, 0x00, 0x01};

        struct attribute_list
        {
            uint16_t bitmap_count;
            uint16_t reserved;
            uint32_t common;
            uint32_t volume;
            uint32_t directory;
            uint32_t file;
            uint32_t fork;
        };

        template <typename T>
        void append_attribute(std::vector<std::byte>& data, const T& value)
        {
            const auto start = data.size();
            data.resize(start + sizeof(value));
            std::memcpy(data.data() + start, &value, sizeof(value));
        }

        // Every path lies on the single root volume, so device, fsid and volume attributes are the same for all of
        // them. They are packed in attribute bit order after a leading length, common attributes first.
        std::optional<std::vector<std::byte>> pack_volume_attributes(const attribute_list& request)
        {
            const auto has_volume_attributes = request.volume != 0;
            if ((request.common & ~SUPPORTED_COMMON_ATTRIBUTES) != 0 || (request.volume & ~SUPPORTED_VOLUME_ATTRIBUTES) != 0 ||
                (has_volume_attributes && (request.volume & ATTR_VOL_INFO) == 0) || request.directory != 0 || request.file != 0 ||
                request.fork != 0)
            {
                return std::nullopt;
            }

            std::vector<std::byte> data(sizeof(uint32_t));
            if (request.common & ATTR_CMN_DEVID)
            {
                append_attribute(data, macos_file_ids::ROOT_FSID[0]);
            }

            if (request.common & ATTR_CMN_FSID)
            {
                append_attribute(data, macos_file_ids::ROOT_FSID);
            }

            if (request.volume & ATTR_VOL_MOUNTFLAGS)
            {
                append_attribute(data, root_filesystem().f_flags);
            }

            if (request.volume & ATTR_VOL_CAPABILITIES)
            {
                constexpr std::array<uint32_t, 4> capabilities{VOL_CAP_FMT_PERSISTENTOBJECTIDS, 0, 0, 0};
                append_attribute(data, capabilities);
                append_attribute(data, capabilities);
            }

            if (request.volume & ATTR_VOL_UUID)
            {
                append_attribute(data, ROOT_VOLUME_UUID);
            }

            const auto length = static_cast<uint32_t>(data.size());
            std::memcpy(data.data(), &length, sizeof(length));
            return data;
        }

        void get_attributes(const macos_syscall_context& c, const std::string& guest_path)
        {
            attribute_list request{};
            if (!c.emu.try_read_memory(get_macos_syscall_argument(c.emu, 1), &request, sizeof(request)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            if (request.common == ATTR_CMN_FULLPATH && request.volume == 0 && request.directory == 0 && request.file == 0)
            {
                write_full_path_attribute(c, guest_path);
                return;
            }

            const auto attributes = pack_volume_attributes(request);
            if (!attributes)
            {
                c.emu_ref.log.error("Unhandled getattrlist(%s, common 0x%x, volume 0x%x)\n", guest_path.c_str(), request.common,
                                    request.volume);
                c.emu_ref.stop();
                return;
            }

            if (!std::filesystem::exists(c.emu_ref.file_system.translate(guest_path)))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
                return;
            }

            const auto buffer_size = get_macos_syscall_argument(c.emu, 3);
            if (buffer_size < attributes->size() ||
                !c.emu.try_write_memory(get_macos_syscall_argument(c.emu, 2), attributes->data(), attributes->size()))
            {
                write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
                return;
            }

            write_macos_syscall_success(c, 0);
        }
    }

    void sys_getattrlist(const macos_syscall_context& c)
    {
        const auto path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        if (!path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (path->empty())
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        get_attributes(c, c.emu_ref.resolve_guest_path(*path));
    }

    void sys_fgetattrlist(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || file->is_standard_stream() || (file->type != fd_type::file && file->type != fd_type::directory))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        get_attributes(c, file->guest_path);
    }

    // The limits of APFS, the file system of the whole emulation root.
    void sys_pathconf(const macos_syscall_context& c)
    {
        constexpr uint64_t pc_name_max = 4;
        constexpr uint64_t pc_path_max = 5;

        const auto path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        const auto name = get_macos_syscall_argument(c.emu, 1);
        if (!path)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (path->empty() || !std::filesystem::exists(c.emu_ref.file_system.translate(c.emu_ref.resolve_guest_path(*path))))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOENT);
            return;
        }

        switch (name)
        {
        case pc_name_max:
            write_macos_syscall_success(c, 255);
            return;
        case pc_path_max:
            write_macos_syscall_success(c, 1024);
            return;
        default:
            c.emu_ref.log.error("Unhandled pathconf(%s, %llu)\n", path->c_str(), static_cast<unsigned long long>(name));
            c.emu_ref.stop();
            return;
        }
    }

    void sys_fstatfs64(const macos_syscall_context& c)
    {
        const auto* file = c.emu_ref.fds.get(static_cast<int>(get_macos_syscall_argument(c.emu, 0)));
        if (file == nullptr || !is_vnode(*file))
        {
            write_macos_syscall_error(c, file == nullptr ? macos_errno::MACOS_EBADF : macos_errno::MACOS_EINVAL);
            return;
        }

        const auto info = root_filesystem();
        if (!c.emu.try_write_memory(get_macos_syscall_argument(c.emu, 1), &info, sizeof(info)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_getfsstat64(const macos_syscall_context& c)
    {
        const auto buffer = get_macos_syscall_argument(c.emu, 0);
        const auto buffer_size = get_macos_syscall_argument(c.emu, 1);

        if (buffer == 0)
        {
            write_macos_syscall_success(c, 1);
            return;
        }

        if (buffer_size < sizeof(macos_statfs64))
        {
            write_macos_syscall_success(c, 0);
            return;
        }

        const auto info = root_filesystem();
        if (!c.emu.try_write_memory(buffer, &info, sizeof(info)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 1);
    }
}
