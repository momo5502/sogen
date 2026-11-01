#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <string_view>

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

    // Only the volume query dyld's FileManager makes on the root volume is answered: device and fsid, then the
    // volume capabilities and UUID, packed in attribute bit order after a leading length.
    void sys_getattrlist(const macos_syscall_context& c)
    {
        constexpr uint32_t attr_cmn_devid = 0x2;
        constexpr uint32_t attr_cmn_fsid = 0x4;
        constexpr uint32_t attr_cmn_fullpath = 0x08000000;
        constexpr uint32_t attr_vol_info = 0x80000000;
        constexpr uint32_t attr_vol_capabilities = 0x20000;
        constexpr uint32_t attr_vol_uuid = 0x40000;
        constexpr uint32_t vol_cap_fmt_persistentobjectids = 0x1;
        constexpr std::array<uint8_t, 16> root_volume_uuid{0x53, 0x4F, 0x47, 0x45, 0x4E, 0x2D, 0x52, 0x4F,
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

#pragma pack(push, 1)

        struct root_volume_attributes
        {
            uint32_t length;
            int32_t device;
            std::array<int32_t, 2> fsid;
            std::array<uint32_t, 4> capabilities;
            std::array<uint32_t, 4> valid;
            std::array<uint8_t, 16> uuid;
        };

#pragma pack(pop)

        const auto path = read_guest_string(c.emu, get_macos_syscall_argument(c.emu, 0));
        attribute_list request{};
        if (!path || !c.emu.try_read_memory(get_macos_syscall_argument(c.emu, 1), &request, sizeof(request)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        if (request.common == attr_cmn_fullpath && request.volume == 0 && request.directory == 0 && request.file == 0)
        {
            write_full_path_attribute(c, *path);
            return;
        }

        if (*path != "/" || request.common != (attr_cmn_devid | attr_cmn_fsid) ||
            request.volume != (attr_vol_info | attr_vol_capabilities | attr_vol_uuid) || request.directory != 0 || request.file != 0)
        {
            c.emu_ref.log.error("Unhandled getattrlist(%s, common 0x%x, volume 0x%x)\n", path->c_str(), request.common, request.volume);
            c.emu_ref.stop();
            return;
        }

        root_volume_attributes attributes{};
        attributes.length = sizeof(attributes);
        attributes.device = macos_file_ids::ROOT_FSID[0];
        attributes.fsid = macos_file_ids::ROOT_FSID;
        attributes.capabilities[0] = vol_cap_fmt_persistentobjectids;
        attributes.valid[0] = vol_cap_fmt_persistentobjectids;
        attributes.uuid = root_volume_uuid;

        const auto buffer_size = get_macos_syscall_argument(c.emu, 3);
        if (buffer_size < sizeof(attributes) ||
            !c.emu.try_write_memory(get_macos_syscall_argument(c.emu, 2), &attributes, sizeof(attributes)))
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
