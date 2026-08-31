#include "../std_include.hpp"

#include "../bsd_syscall_dispatcher.hpp"
#include "../bsd_syscall_numbers.hpp"
#include "../macos_emulator.hpp"
#include "../module/dyld_cache_pager.hpp"
#include "../module/dyld_cache_slide.hpp"
#include "../macos_file_identity.hpp"
#include "../macos_stat.hpp"
#include "../macos_syscall_utils.hpp"

#include <guest/guest_memory_object.hpp>

#include <cinttypes>
#include <ranges>
#include <cstring>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{
    using namespace macos_errno;
    using namespace macos_syscalls;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        constexpr size_t MACOS_ATTR_RETURNED_ATTRS_SIZE = 20;

        uint32_t object_type_of(const std::filesystem::path& host_path)
        {
            std::error_code error{};

            if (std::filesystem::is_symlink(host_path, error) && !error)
            {
                return macos_attr::OBJ_TYPE_VLNK;
            }

            if (std::filesystem::is_directory(host_path, error) && !error)
            {
                return macos_attr::OBJ_TYPE_VDIR;
            }

            return macos_attr::OBJ_TYPE_VREG;
        }

        void append(std::vector<uint8_t>& out, const void* data, const size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        // read_guest_string reads exactly `size` elements when it is given a bound -- it does not stop
        // at the terminator -- so a bounded read has to be trimmed, or the path carries a thousand
        // trailing NULs into the identity table and never matches the same file again.
        std::string read_bounded_guest_path(const macos_syscall_context& c, const uint64_t address)
        {
            auto path = read_guest_string<char>(c.emu_ref.memory, address, MACOS_PATH_MAX);
            path.resize(std::min(path.find('\0'), path.size()));
            return path;
        }

        // A variable-length attribute is an attrreference_t in the fixed section -- an offset measured
        // from the reference's own address, plus a length -- with the bytes appended after every other
        // fixed field. The kernel lays them out in that order and the caller's unpacker walks it that
        // way, so the reference is written now and patched once the data lands.
        size_t append_attr_reference_placeholder(std::vector<uint8_t>& out)
        {
            const auto offset = out.size();
            const std::array<uint32_t, 2> placeholder{0, 0};
            append(out, placeholder.data(), sizeof(placeholder));
            return offset;
        }

        void patch_attr_reference(std::vector<uint8_t>& out, const size_t reference_offset, const std::string_view value)
        {
            const auto data_offset = static_cast<int32_t>(out.size() - reference_offset);
            const auto data_length = static_cast<uint32_t>(value.size() + 1);

            std::memcpy(out.data() + reference_offset, &data_offset, sizeof(data_offset));
            std::memcpy(out.data() + reference_offset + sizeof(data_offset), &data_length, sizeof(data_length));

            append(out, value.data(), value.size());
            out.push_back(0);
        }

        // struct user64_timespec: two 64-bit words, which is what the kernel packs for a 64-bit caller.
        void append_timespec(std::vector<uint8_t>& out, const int64_t seconds)
        {
            const std::array<int64_t, 2> value{seconds, 0};
            append(out, value.data(), sizeof(value));
        }

        uint64_t host_file_size(const std::filesystem::path& host_path)
        {
            std::error_code error{};
            const auto size = std::filesystem::file_size(host_path, error);
            return error ? 0 : size;
        }

        size_t directory_entry_count(const std::filesystem::path& host_path)
        {
            std::error_code error{};
            size_t count = 0;
            for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(host_path, error))
            {
                ++count;
            }

            return error ? 0 : count;
        }

        int64_t host_modification_seconds(const std::filesystem::path& host_path)
        {
            std::error_code error{};
            const auto written = std::filesystem::last_write_time(host_path, error);
            if (error)
            {
                return 0;
            }

            const auto since_epoch = std::chrono::duration_cast<std::chrono::seconds>(written.time_since_epoch());
            return since_epoch.count();
        }

        void copy_fixed(std::span<char> field, const std::string_view value)
        {
            const auto length = std::min(value.size(), field.size() - 1);
            std::memcpy(field.data(), value.data(), length);
            field[length] = '\0';
        }
    }

    void sys_crossarch_trap(const macos_syscall_context& c)
    {
        // dyld executes this on a native arm64 launch; the Rosetta AOT path it serves is inert here, and
        // dyld only tests it for failure.
        write_macos_syscall_result(c, 0);
    }

    void sys_csrctl(const macos_syscall_context& c)
    {
        // A stock Mac has SIP enabled, so csr_check() fails for every capability dyld asks about.
        // Anything else tells dyld it is on an Apple-internal or SIP-disabled system and changes the
        // paths it takes.
        write_macos_syscall_error(c, MACOS_EPERM);
    }

    void sys_mremap_encrypted(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_ENOSYS);
    }

    void sys_map_with_linking_np(const macos_syscall_context& c)
    {
        // DYLD_PAGEIN_LINKING=0 is forced into the guest environment, so dyld applies fixups itself and
        // never needs the kernel's page-in-linking service. Failing it keeps that contract observable
        // instead of silently diverging.
        write_macos_syscall_error(c, MACOS_ENOSYS);
    }

    void sys_platform_syscall(const macos_syscall_context& c)
    {
        // The operation code arrives in x3. Those that exist on arm64 are legacy 32-bit TLS and
        // cache-maintenance assists; the softmmu is coherent and the thread pointer already lives in
        // TPIDRRO_EL0, so each is a no-op here. Logged so a bring-up run can tell whether that
        // assumption is ever exercised.
        c.emu_ref.log.print(color::gray, "platform syscall op=0x%" PRIx64 "\n", get_macos_syscall_argument(c, 3));
        write_macos_syscall_result(c, 0);
    }

    void sys_getfsstat64(const macos_syscall_context& c)
    {
        const auto buffer = get_macos_syscall_argument(c, 0);
        const auto buffer_size = get_macos_syscall_argument(c, 1);

        if (buffer == 0)
        {
            write_macos_syscall_result(c, 1);
            return;
        }

        if (buffer_size < sizeof(macos_statfs64))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        macos_statfs64 entry{};
        entry.f_bsize = static_cast<uint32_t>(MACOS_PAGE_SIZE);
        entry.f_iosize = static_cast<int32_t>(MACOS_PAGE_SIZE);
        entry.f_fsid[0] = static_cast<int32_t>(MACOS_SYNTHETIC_FSID_DEV);
        entry.f_fsid[1] = static_cast<int32_t>(MACOS_SYNTHETIC_FSID_VFSTYPE);
        entry.f_type = MACOS_SYNTHETIC_FSID_VFSTYPE;
        copy_fixed(entry.f_fstypename, "apfs");
        copy_fixed(entry.f_mntonname, "/");
        copy_fixed(entry.f_mntfromname, "/dev/disk1s1");

        if (!c.emu_ref.memory.try_write_memory(buffer, &entry, sizeof(entry)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 1);
    }

    void sys_fsgetpath(const macos_syscall_context& c)
    {
        const auto out_buffer = get_macos_syscall_argument(c, 0);
        const auto out_size = get_macos_syscall_argument(c, 1);
        const auto fsid_pointer = get_macos_syscall_argument(c, 2);
        const auto object_id = get_macos_syscall_argument(c, 3);

        std::array<int32_t, 2> fsid{};
        if (!c.emu_ref.memory.try_read_memory(fsid_pointer, fsid.data(), sizeof(fsid)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        const auto resolved = c.emu_ref.identities.resolve(static_cast<uint32_t>(fsid[0]), static_cast<uint32_t>(fsid[1]), object_id);
        if (!resolved)
        {
            write_macos_syscall_error(c, MACOS_ENOENT);
            return;
        }

        if (resolved->size() + 1 > out_size)
        {
            write_macos_syscall_error(c, MACOS_ENOSPC);
            return;
        }

        if (!c.emu_ref.memory.try_write_memory(out_buffer, resolved->c_str(), resolved->size() + 1))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, static_cast<int64_t>(resolved->size()) + 1);
    }

    // Shared by getattrlist and fgetattrlist, which differ only in how the target is named. One encoder
    // means a caller cannot get a different answer depending on which of the two it used.
    static void answer_attrlist(const macos_syscall_context& c, const std::string& guest_path)
    {
        const auto request_pointer = get_macos_syscall_argument(c, 1);
        const auto out_buffer = get_macos_syscall_argument(c, 2);
        const auto out_size = get_macos_syscall_argument(c, 3);

        macos_attrlist request{};
        if (!c.emu_ref.memory.try_read_memory(request_pointer, &request, sizeof(request)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        if (request.bitmapcount != 5 || (request.commonattr & ~macos_attr::CMN_SUPPORTED) != 0 ||
            (request.volattr & ~macos_attr::VOL_SUPPORTED) != 0 || (request.dirattr & ~macos_attr::DIR_SUPPORTED) != 0 ||
            (request.fileattr & ~macos_attr::FILE_SUPPORTED) != 0 || (request.forkattr & ~macos_attr::CMNEXT_SUPPORTED) != 0)
        {
            c.emu_ref.log.print(color::yellow,
                                "getattrlist(%s): unsupported request bitmapcount=%u common=0x%08x vol=0x%08x dir=0x%08x file=0x%08x "
                                "fork=0x%08x (unhandled common bits 0x%08x, vol bits 0x%08x)\n",
                                guest_path.c_str(), request.bitmapcount, request.commonattr, request.volattr, request.dirattr,
                                request.fileattr, request.forkattr, request.commonattr & ~macos_attr::CMN_SUPPORTED,
                                request.volattr & ~macos_attr::VOL_SUPPORTED);
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto host_path = c.emu_ref.file_sys.translate(guest_path);

        std::error_code error{};
        if (!std::filesystem::exists(host_path, error) || error)
        {
            write_macos_syscall_error(c, MACOS_ENOENT);
            return;
        }

        const auto identity = c.emu_ref.identities.acquire(guest_path);
        const auto is_directory = object_type_of(host_path) == macos_attr::OBJ_TYPE_VDIR;
        const auto modified = host_modification_seconds(host_path);
        const auto leaf = std::filesystem::path{guest_path}.filename().string();
        const auto parent_path = std::filesystem::path{guest_path}.parent_path().generic_string();
        const auto parent = c.emu_ref.identities.acquire(parent_path.empty() ? std::string{"/"} : parent_path);

        std::vector<uint8_t> encoded{};
        encoded.resize(sizeof(uint32_t));

        if ((request.commonattr & macos_attr::CMN_RETURNED_ATTRS) != 0)
        {
            // What is actually in the buffer, which is not what was asked for: the kernel emits the
            // directory group only for a directory and the file group only for anything else, and a
            // caller that walks the buffer by this set would skip or misread a group it was told about
            // and did not get.
            const std::array<uint32_t, 5> returned{
                request.commonattr, request.volattr, is_directory ? request.dirattr : 0u, is_directory ? 0u : request.fileattr,
                request.forkattr,
            };
            append(encoded, returned.data(), MACOS_ATTR_RETURNED_ATTRS_SIZE);
        }

        // Attributes are emitted in increasing bit order within the common group, which is the order
        // the caller's own unpacker walks. Variable-length ones reserve their attrreference here and
        // append their bytes after the whole fixed section, so their offsets are patched at the end.
        size_t name_reference = 0;
        if ((request.commonattr & macos_attr::CMN_NAME) != 0)
        {
            name_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.commonattr & macos_attr::CMN_DEVID) != 0)
        {
            append(encoded, &identity.fsid_dev, sizeof(identity.fsid_dev));
        }

        if ((request.commonattr & macos_attr::CMN_FSID) != 0)
        {
            const std::array<uint32_t, 2> value{identity.fsid_dev, identity.fsid_vfstype};
            append(encoded, value.data(), sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_OBJTYPE) != 0)
        {
            const auto value = object_type_of(host_path);
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_OBJID) != 0)
        {
            const std::array<uint32_t, 2> value{static_cast<uint32_t>(identity.object_id), 0};
            append(encoded, value.data(), sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_PAROBJID) != 0)
        {
            const std::array<uint32_t, 2> value{static_cast<uint32_t>(parent.object_id), 0};
            append(encoded, value.data(), sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_SCRIPT) != 0)
        {
            // kTextEncodingMacUnicode is what an APFS volume reports for every name.
            constexpr uint32_t text_encoding_unicode = 0x100;
            append(encoded, &text_encoding_unicode, sizeof(text_encoding_unicode));
        }

        // Every timestamp is the host file's modification time. A synthetic root mirrors host files, so
        // that is the one real time available; inventing a distinct creation or access time would be a
        // fabrication a caller could catch by comparing them against the same file's stat.
        if ((request.commonattr & macos_attr::CMN_CRTIME) != 0)
        {
            append_timespec(encoded, modified);
        }

        if ((request.commonattr & macos_attr::CMN_MODTIME) != 0)
        {
            append_timespec(encoded, modified);
        }

        if ((request.commonattr & macos_attr::CMN_CHGTIME) != 0)
        {
            append_timespec(encoded, modified);
        }

        if ((request.commonattr & macos_attr::CMN_ACCTIME) != 0)
        {
            append_timespec(encoded, modified);
        }

        if ((request.commonattr & macos_attr::CMN_FNDRINFO) != 0)
        {
            // 32 bytes of Finder info. A file that has never been touched by Finder carries zeroes, and
            // nothing in the emulation root ever has been.
            const std::array<uint8_t, 32> value{};
            append(encoded, value.data(), value.size());
        }

        if ((request.commonattr & macos_attr::CMN_OWNERID) != 0)
        {
            const auto value = c.proc.uid;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_GRPID) != 0)
        {
            const auto value = c.proc.gid;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_ACCESSMASK) != 0)
        {
            // The emulation root is presented read-only, matching the mount flags below.
            constexpr uint32_t file_mode = 0100555;
            constexpr uint32_t directory_mode = 0040555;
            const uint32_t value = is_directory ? directory_mode : file_mode;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_FLAGS) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_GEN_COUNT) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_DOCUMENT_ID) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_USERACCESS) != 0)
        {
            constexpr uint32_t r_ok = 0x04;
            constexpr uint32_t x_ok = 0x01;
            constexpr uint32_t value = r_ok | x_ok;
            append(encoded, &value, sizeof(value));
        }

        if ((request.commonattr & macos_attr::CMN_FILEID) != 0)
        {
            append(encoded, &identity.object_id, sizeof(identity.object_id));
        }

        if ((request.commonattr & macos_attr::CMN_PARENTID) != 0)
        {
            append(encoded, &parent.object_id, sizeof(parent.object_id));
        }

        // dyld asks for the full path on the executable while working out what it is running, and
        // refusing it makes it halt before a single initialiser runs.
        size_t fullpath_reference = 0;
        if ((request.commonattr & macos_attr::CMN_FULLPATH) != 0)
        {
            fullpath_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.commonattr & macos_attr::CMN_ADDEDTIME) != 0)
        {
            append_timespec(encoded, modified);
        }

        if ((request.commonattr & macos_attr::CMN_DATA_PROTECT_FLAGS) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        // The volume group follows the common one, in the same increasing bit order. Every size below
        // describes the emulation root as one read-only APFS volume, which is exactly what it is; the
        // free-space answers are zero because nothing a guest writes ever reaches the host.
        constexpr uint64_t volume_size = 0x10000000000ULL;

        if ((request.volattr & macos_attr::VOL_FSTYPE) != 0)
        {
            const auto value = static_cast<uint32_t>(MACOS_SYNTHETIC_FSID_VFSTYPE);
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_SIGNATURE) != 0)
        {
            // 'BD' is what every HFS-lineage volume reports and what APFS keeps reporting.
            const uint32_t value = 0x4244;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_SIZE) != 0)
        {
            append(encoded, &volume_size, sizeof(volume_size));
        }

        if ((request.volattr & macos_attr::VOL_SPACEFREE) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_SPACEAVAIL) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_MINALLOCATION) != 0)
        {
            const uint64_t value = MACOS_PAGE_SIZE;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_ALLOCATIONCLUMP) != 0)
        {
            const uint64_t value = MACOS_PAGE_SIZE;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_IOBLOCKSIZE) != 0)
        {
            const auto value = static_cast<uint32_t>(MACOS_PAGE_SIZE);
            append(encoded, &value, sizeof(value));
        }

        // Counting every object under the root would walk the whole host tree on a call a guest makes
        // to decide how to draw a progress bar, so these report the unknown-count answer a volume with
        // no object index gives.
        if ((request.volattr & macos_attr::VOL_OBJCOUNT) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_FILECOUNT) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_DIRCOUNT) != 0)
        {
            const uint32_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_MAXOBJCOUNT) != 0)
        {
            const uint32_t value = 0xFFFFFFFF;
            append(encoded, &value, sizeof(value));
        }

        size_t mountpoint_reference = 0;
        if ((request.volattr & macos_attr::VOL_MOUNTPOINT) != 0)
        {
            mountpoint_reference = append_attr_reference_placeholder(encoded);
        }

        size_t volume_name_reference = 0;
        if ((request.volattr & macos_attr::VOL_NAME) != 0)
        {
            volume_name_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.volattr & macos_attr::VOL_MOUNTFLAGS) != 0)
        {
            constexpr uint32_t MNT_RDONLY = 0x00000001;

            // The root is presented read-only because it is: guest writes never reach the analyst's
            // files, and telling a guest otherwise invites it to try.
            constexpr uint32_t flags = MNT_RDONLY;
            append(encoded, &flags, sizeof(flags));
        }

        size_t mounted_device_reference = 0;
        if ((request.volattr & macos_attr::VOL_MOUNTEDDEVICE) != 0)
        {
            mounted_device_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.volattr & macos_attr::VOL_ENCODINGSUSED) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_CAPABILITIES) != 0)
        {
            // vol_capabilities_attr_t: what the volume supports, then which of those answers are
            // meaningful. Case-preserving but not case-sensitive is what an ordinary APFS volume reports,
            // and it is what the host filesystem underneath actually does -- claiming case sensitivity
            // would make a guest stop folding names the host still folds.
            constexpr uint32_t FMT_PERSISTENTOBJECTIDS = 0x00000001;
            constexpr uint32_t FMT_SYMBOLICLINKS = 0x00000002;
            constexpr uint32_t FMT_HARDLINKS = 0x00000004;
            constexpr uint32_t FMT_SPARSE_FILES = 0x00000040;
            constexpr uint32_t FMT_CASE_SENSITIVE = 0x00000100;
            constexpr uint32_t FMT_CASE_PRESERVING = 0x00000200;
            constexpr uint32_t FMT_FAST_STATFS = 0x00000400;
            constexpr uint32_t FMT_2TB_FILESIZE = 0x00000800;
            constexpr uint32_t INT_ATTRLIST = 0x00000002;

            constexpr uint32_t format_capabilities = FMT_PERSISTENTOBJECTIDS | FMT_SYMBOLICLINKS | FMT_HARDLINKS | FMT_SPARSE_FILES |
                                                     FMT_CASE_PRESERVING | FMT_FAST_STATFS | FMT_2TB_FILESIZE;
            constexpr uint32_t format_valid = format_capabilities | FMT_CASE_SENSITIVE;

            const std::array<uint32_t, 8> capabilities{
                format_capabilities, INT_ATTRLIST, 0, 0, format_valid, INT_ATTRLIST, 0, 0,
            };
            append(encoded, capabilities.data(), sizeof(capabilities));
        }

        if ((request.volattr & macos_attr::VOL_UUID) != 0)
        {
            // One volume, one identity, stable across runs so a guest that caches it by UUID keeps
            // matching. Version 4 variant 1, with the rest naming what it is.
            constexpr std::array<uint8_t, 16> uuid{0x50, 0x6f, 0x67, 0x65, 0x6e, 0x00, 0x40, 0x00,
                                                   0x80, 0x00, 0x72, 0x6f, 0x6f, 0x74, 0x66, 0x73};
            append(encoded, uuid.data(), uuid.size());
        }

        if ((request.volattr & macos_attr::VOL_QUOTA_SIZE) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.volattr & macos_attr::VOL_RESERVED_SIZE) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        // vol_attributes_attr_t: which attributes the volume can answer, then which of those it stores
        // natively. This emulator computes every one of them, so the two sets are the same, and both are
        // exactly what answer_attrlist above encodes -- an attribute missing here is one a caller would
        // be told to expect and then not get.
        if ((request.volattr & macos_attr::VOL_ATTRIBUTES) != 0)
        {
            const std::array<uint32_t, 10> attributes{
                macos_attr::CMN_SUPPORTED,    macos_attr::VOL_SUPPORTED,    macos_attr::DIR_SUPPORTED, macos_attr::FILE_SUPPORTED,
                macos_attr::CMNEXT_SUPPORTED, macos_attr::CMN_SUPPORTED,    macos_attr::VOL_SUPPORTED, macos_attr::DIR_SUPPORTED,
                macos_attr::FILE_SUPPORTED,   macos_attr::CMNEXT_SUPPORTED,
            };
            append(encoded, attributes.data(), sizeof(attributes));
        }

        // The kernel emits the directory group only for a directory and the file group only for anything
        // else, so a caller that asks for both gets exactly one of them back.
        if (is_directory)
        {
            if ((request.dirattr & macos_attr::DIR_LINKCOUNT) != 0)
            {
                const uint32_t value = 2;
                append(encoded, &value, sizeof(value));
            }

            if ((request.dirattr & macos_attr::DIR_ENTRYCOUNT) != 0)
            {
                const auto value = static_cast<uint32_t>(directory_entry_count(host_path));
                append(encoded, &value, sizeof(value));
            }

            if ((request.dirattr & macos_attr::DIR_MOUNTSTATUS) != 0)
            {
                // DIR_MNTSTATUS_MNTPOINT / DIR_MNTSTATUS_TRIGGER: the emulation root is one volume with
                // no submounts and no trigger points.
                const uint32_t value = 0;
                append(encoded, &value, sizeof(value));
            }

            if ((request.dirattr & macos_attr::DIR_ALLOCSIZE) != 0)
            {
                const uint64_t value = 0;
                append(encoded, &value, sizeof(value));
            }

            if ((request.dirattr & macos_attr::DIR_IOBLOCKSIZE) != 0)
            {
                const auto value = static_cast<uint32_t>(MACOS_PAGE_SIZE);
                append(encoded, &value, sizeof(value));
            }

            if ((request.dirattr & macos_attr::DIR_DATALENGTH) != 0)
            {
                const uint64_t value = 0;
                append(encoded, &value, sizeof(value));
            }
        }
        else
        {
            const auto size = host_file_size(host_path);

            if ((request.fileattr & macos_attr::FILE_LINKCOUNT) != 0)
            {
                const uint32_t value = 1;
                append(encoded, &value, sizeof(value));
            }

            if ((request.fileattr & macos_attr::FILE_TOTALSIZE) != 0)
            {
                append(encoded, &size, sizeof(size));
            }

            if ((request.fileattr & macos_attr::FILE_ALLOCSIZE) != 0)
            {
                append(encoded, &size, sizeof(size));
            }

            if ((request.fileattr & macos_attr::FILE_IOBLOCKSIZE) != 0)
            {
                const auto value = static_cast<uint32_t>(MACOS_PAGE_SIZE);
                append(encoded, &value, sizeof(value));
            }

            if ((request.fileattr & macos_attr::FILE_DEVTYPE) != 0)
            {
                const uint32_t value = 0;
                append(encoded, &value, sizeof(value));
            }

            if ((request.fileattr & macos_attr::FILE_DATALENGTH) != 0)
            {
                append(encoded, &size, sizeof(size));
            }

            if ((request.fileattr & macos_attr::FILE_DATAALLOCSIZE) != 0)
            {
                append(encoded, &size, sizeof(size));
            }

            // Nothing in the emulation root carries a resource fork.
            if ((request.fileattr & macos_attr::FILE_RSRCLENGTH) != 0)
            {
                const uint64_t value = 0;
                append(encoded, &value, sizeof(value));
            }

            if ((request.fileattr & macos_attr::FILE_RSRCALLOCSIZE) != 0)
            {
                const uint64_t value = 0;
                append(encoded, &value, sizeof(value));
            }
        }

        size_t relpath_reference = 0;
        if ((request.forkattr & macos_attr::CMNEXT_RELPATH) != 0)
        {
            relpath_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.forkattr & macos_attr::CMNEXT_PRIVATESIZE) != 0)
        {
            const int64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.forkattr & macos_attr::CMNEXT_LINKID) != 0)
        {
            append(encoded, &identity.object_id, sizeof(identity.object_id));
        }

        size_t nofirmlink_reference = 0;
        if ((request.forkattr & macos_attr::CMNEXT_NOFIRMLINKPATH) != 0)
        {
            nofirmlink_reference = append_attr_reference_placeholder(encoded);
        }

        if ((request.forkattr & macos_attr::CMNEXT_REALDEVID) != 0)
        {
            append(encoded, &identity.fsid_dev, sizeof(identity.fsid_dev));
        }

        if ((request.forkattr & macos_attr::CMNEXT_REALFSID) != 0)
        {
            const std::array<uint32_t, 2> value{identity.fsid_dev, identity.fsid_vfstype};
            append(encoded, value.data(), sizeof(value));
        }

        if ((request.forkattr & macos_attr::CMNEXT_CLONEID) != 0)
        {
            append(encoded, &identity.object_id, sizeof(identity.object_id));
        }

        if ((request.forkattr & macos_attr::CMNEXT_EXT_FLAGS) != 0)
        {
            // EF_MAY_SHARE_BLOCKS / EF_NO_XATTRS / EF_IS_SYNC_ROOT / EF_IS_PURGEABLE and friends: none of
            // them describe a file in a synthetic root mirroring host files.
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        if ((request.forkattr & macos_attr::CMNEXT_RECURSIVE_GENCOUNT) != 0)
        {
            const uint64_t value = 0;
            append(encoded, &value, sizeof(value));
        }

        // Variable-length data follows the whole fixed section, in the order the references appear in it.
        if ((request.commonattr & macos_attr::CMN_NAME) != 0)
        {
            patch_attr_reference(encoded, name_reference, leaf);
        }

        if ((request.commonattr & macos_attr::CMN_FULLPATH) != 0)
        {
            patch_attr_reference(encoded, fullpath_reference, guest_path);
        }

        if ((request.volattr & macos_attr::VOL_MOUNTPOINT) != 0)
        {
            patch_attr_reference(encoded, mountpoint_reference, "/");
        }

        if ((request.volattr & macos_attr::VOL_NAME) != 0)
        {
            patch_attr_reference(encoded, volume_name_reference, "Macintosh HD");
        }

        if ((request.volattr & macos_attr::VOL_MOUNTEDDEVICE) != 0)
        {
            patch_attr_reference(encoded, mounted_device_reference, "/dev/disk1s1");
        }

        // Relative to the volume root, which for this emulator is the guest path without its leading
        // slash; there are no firmlinks in a synthetic root, so the no-firmlink path is the same one.
        if ((request.forkattr & macos_attr::CMNEXT_RELPATH) != 0)
        {
            patch_attr_reference(encoded, relpath_reference, std::string_view{guest_path}.substr(guest_path.starts_with('/') ? 1 : 0));
        }

        if ((request.forkattr & macos_attr::CMNEXT_NOFIRMLINKPATH) != 0)
        {
            patch_attr_reference(encoded, nofirmlink_reference, guest_path);
        }

        const auto total = static_cast<uint32_t>(encoded.size());
        std::memcpy(encoded.data(), &total, sizeof(total));

        if (encoded.size() > out_size)
        {
            write_macos_syscall_error(c, MACOS_ERANGE);
            return;
        }

        if (!c.emu_ref.memory.try_write_memory(out_buffer, encoded.data(), encoded.size()))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // pathconf/fpathconf report the limits of the filesystem behind a path. Every value below is what
    // an APFS volume on this host reports; the emulation root is one such volume, mirrored read-only.
    namespace
    {
        std::optional<int64_t> pathconf_value(const macos_syscall_context& c, const int32_t name)
        {
            switch (name)
            {
            case 1: // _PC_LINK_MAX
                return 32767;
            case 2: // _PC_MAX_CANON
            case 3: // _PC_MAX_INPUT
                return 1024;
            case 4: // _PC_NAME_MAX
                return 255;
            case 5: // _PC_PATH_MAX
                return static_cast<int64_t>(MACOS_PATH_MAX);
            case 6: // _PC_PIPE_BUF
                return 512;
            case 7: // _PC_CHOWN_RESTRICTED
            case 8: // _PC_NO_TRUNC
                return 1;
            case 9: // _PC_VDISABLE
                return 0xff;
            case 10: // _PC_NAME_CHARS_MAX
                return 255;
            case 11: // _PC_CASE_SENSITIVE -- case-preserving but not case-sensitive, as ATTR_VOL_CAPABILITIES also reports
                return 0;
            case 12: // _PC_CASE_PRESERVING
                return 1;
            case 13: // _PC_EXTENDED_SECURITY_NP
                return 1;
            case 14: // _PC_AUTH_OPAQUE_NP
                return 0;
            case 15: // _PC_2_SYMLINKS
                return 1;
            case 16: // _PC_ALLOC_SIZE_MIN
                return static_cast<int64_t>(MACOS_PAGE_SIZE);
            case 17: // _PC_ASYNC_IO
                return 1;
            case 18: // _PC_FILESIZEBITS
                return 64;
            case 19: // _PC_PRIO_IO
                return 0;
            case 20: // _PC_REC_INCR_XFER_SIZE
                return static_cast<int64_t>(MACOS_PAGE_SIZE);
            case 21: // _PC_REC_MAX_XFER_SIZE
                return 1048576;
            case 22: // _PC_REC_MIN_XFER_SIZE
            case 23: // _PC_REC_XFER_ALIGN
                return static_cast<int64_t>(MACOS_PAGE_SIZE);
            case 24: // _PC_SYMLINK_MAX
                return static_cast<int64_t>(MACOS_PATH_MAX);
            case 25: // _PC_SYNC_IO
                return 0;
            case 26: // _PC_XATTR_SIZE_BITS
                return 64;
            case 27: // _PC_MIN_HOLE_SIZE
                return static_cast<int64_t>(MACOS_PAGE_SIZE);
            default:
                break;
            }

            c.emu_ref.log.warn("pathconf name %d has no answer in sogen\n", name);
            return std::nullopt;
        }
    }

    void sys_pathconf(const macos_syscall_context& c)
    {
        const auto value = pathconf_value(c, static_cast<int32_t>(get_macos_syscall_argument(c, 1)));
        if (!value)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        write_macos_syscall_result(c, *value);
    }

    void sys_fpathconf(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(get_macos_syscall_argument(c, 0));
        if (c.proc.fds.get(fd) == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        const auto value = pathconf_value(c, static_cast<int32_t>(get_macos_syscall_argument(c, 1)));
        if (!value)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        write_macos_syscall_result(c, *value);
    }

    void sys_fsctl(const macos_syscall_context& c)
    {
        // dyld asks the filesystem for volume capabilities to decide whether it can trust a path's
        // case-sensitivity. Reporting "the volume does not answer that" sends it down the conservative
        // path, which is the correct one for a synthetic root.
        write_macos_syscall_error(c, MACOS_ENOTTY);
    }

    namespace
    {
        // xnu's <sys/shared_region.h>, which is not in the SDK. Both layouts were measured from what
        // dyld actually passes on this host: the mappings array is exactly count * 48 bytes and ends
        // where the descriptor array begins, and the first entries decode to the cache's own regions.
        constexpr size_t SHARED_FILE_ENTRY_SIZE = 12;
        constexpr size_t SHARED_MAPPING_ENTRY_SIZE = 48;

        struct shared_region_file
        {
            int32_t fd{};
            uint32_t mapping_count{};
            uint32_t slide{};
        };

        struct shared_region_mapping
        {
            uint64_t address{};
            uint64_t size{};
            uint64_t file_offset{};
            uint64_t slide_size{};
            uint64_t slide_start{};
            int32_t max_protection{};
            int32_t init_protection{};
        };

        bool read_shared_region_file(const macos_syscall_context& c, const uint64_t address, shared_region_file& out)
        {
            std::array<uint8_t, SHARED_FILE_ENTRY_SIZE> raw{};
            if (!c.emu_ref.memory.try_read_memory(address, raw.data(), raw.size()))
            {
                return false;
            }

            std::memcpy(&out.fd, raw.data(), sizeof(out.fd));
            std::memcpy(&out.mapping_count, raw.data() + 4, sizeof(out.mapping_count));
            std::memcpy(&out.slide, raw.data() + 8, sizeof(out.slide));
            return true;
        }

        bool read_shared_region_mapping(const macos_syscall_context& c, const uint64_t address, shared_region_mapping& out)
        {
            std::array<uint8_t, SHARED_MAPPING_ENTRY_SIZE> raw{};
            if (!c.emu_ref.memory.try_read_memory(address, raw.data(), raw.size()))
            {
                return false;
            }

            std::memcpy(&out.address, raw.data(), sizeof(out.address));
            std::memcpy(&out.size, raw.data() + 8, sizeof(out.size));
            std::memcpy(&out.file_offset, raw.data() + 16, sizeof(out.file_offset));
            std::memcpy(&out.slide_size, raw.data() + 24, sizeof(out.slide_size));
            std::memcpy(&out.slide_start, raw.data() + 32, sizeof(out.slide_start));
            std::memcpy(&out.max_protection, raw.data() + 40, sizeof(out.max_protection));
            std::memcpy(&out.init_protection, raw.data() + 44, sizeof(out.init_protection));
            return true;
        }

        // The protection words carry more than read/write/execute, which is why the check below cannot
        // simply demand a value under 8. Measured on this host: 0x20 marks a region the cache slides,
        // 0x40 one whose pointers are not authenticated, and 0x200 appears only in max_protection.
        // macos_prot_to_permission reads the low three bits and ignores the rest, so nothing needs to
        // strip them. The protection half of the check below is defence against a future layout drift
        // rather than against anything seen today, and no test pins it; the geometry half is what
        // refuses a mapping that would alias file bytes at an arbitrary address.
        constexpr int32_t SHARED_REGION_PROTECTION_FLAGS = 0x260;

        // The dynamic region is a page the kernel synthesises at the top of the shared region; it is in
        // no cache file. dyld finds it at cache_base + header[0x1f0] and validates it by comparing the
        // first fourteen bytes against "dyld_data    v", so a page of zeroes reads as "absent" and
        // DyldSharedCache::dynamicRegion() returns null -- which its caller then dereferences.
        //
        // Every offset below is read straight out of dyld's own accessors: version() takes a signed byte
        // at 0x0e and subtracts '0', setDyldCacheFileID stores a pair at 0x10, osCryptexPath and
        // cachePath are u32 offsets at 0x20 and 0x24 with zero meaning "none", and setCachePath places
        // the string at 0x50 when there is no cryptex path.
        constexpr uint64_t CACHE_HEADER_DYNAMIC_REGION_OFFSET = 0x1F0;
        constexpr size_t DYNAMIC_REGION_SIZE = 0x4000;
        constexpr size_t DYNAMIC_REGION_CACHE_PATH_OFFSET = 0x50;
        constexpr std::string_view DYNAMIC_REGION_MAGIC = "dyld_data    v1";

        bool publish_dynamic_region(const macos_syscall_context& c, const uint64_t cache_base, const std::string& cache_path)
        {
            uint64_t region_offset = 0;
            if (!c.emu_ref.memory.try_read_memory(cache_base + CACHE_HEADER_DYNAMIC_REGION_OFFSET, &region_offset, sizeof(region_offset)) ||
                region_offset == 0)
            {
                return true;
            }

            const auto region = cache_base + region_offset;
            if (!c.emu_ref.memory.overlaps_mapped_region(region, DYNAMIC_REGION_SIZE) &&
                !c.emu_ref.memory.allocate_memory(region, DYNAMIC_REGION_SIZE, memory_permission::read_write))
            {
                return false;
            }

            std::vector<uint8_t> page(DYNAMIC_REGION_SIZE, 0);
            std::ranges::copy(DYNAMIC_REGION_MAGIC, reinterpret_cast<char*>(page.data()));

            const auto identity = c.emu_ref.identities.acquire(cache_path);
            const auto packed_fsid = identity.packed_fsid();
            std::memcpy(page.data() + 0x10, &packed_fsid, sizeof(packed_fsid));
            std::memcpy(page.data() + 0x18, &identity.object_id, sizeof(identity.object_id));

            if (cache_path.size() + 1 > DYNAMIC_REGION_SIZE - DYNAMIC_REGION_CACHE_PATH_OFFSET)
            {
                return false;
            }

            const auto path_offset = static_cast<uint32_t>(DYNAMIC_REGION_CACHE_PATH_OFFSET);
            std::memcpy(page.data() + 0x24, &path_offset, sizeof(path_offset));
            std::ranges::copy(cache_path, reinterpret_cast<char*>(page.data()) + DYNAMIC_REGION_CACHE_PATH_OFFSET);

            return c.emu_ref.memory.try_write_memory(region, page.data(), page.size());
        }

        bool mapping_is_plausible(const shared_region_mapping& mapping)
        {
            constexpr int32_t KNOWN = 7 | SHARED_REGION_PROTECTION_FLAGS;

            return mapping.size != 0 && (mapping.address % MACOS_PAGE_SIZE) == 0 && (mapping.size % MACOS_PAGE_SIZE) == 0 &&
                   (mapping.file_offset % MACOS_PAGE_SIZE) == 0 && (mapping.init_protection & ~KNOWN) == 0 &&
                   (mapping.max_protection & ~KNOWN) == 0;
        }
    }

    // dyld mmaps every subcache first and then asks the kernel to install them at the addresses the
    // cache was built for. Refusing the call is not an option that leaves the cache usable: dyld falls
    // straight back to searching the filesystem and reports "no dyld cache".
    struct pending_slide
    {
        uint64_t target{};
        size_t length{};
        uint64_t slide_start{};
        uint64_t slide_size{};
    };

    namespace
    {
        // Hands every unmappable cache region to the pager, with a fixup that rebases exactly the chunk
        // being materialised.
        //
        // The chunk's bytes are rebased in the pager's own buffer, before it is mapped, because
        // apply_dyld_cache_slide_info writes through guest memory and the chunk does not exist there
        // yet. So the fixup stages the buffer at its final address in a scratch mapping, rebases it, and
        // copies the result back -- the alternative being a second implementation of the chain walk that
        // works on a span, which would then be the one that drifts.
        void install_lazy_cache_pager(macos_emulator& emu, std::vector<dyld_cache_backing_range> ranges,
                                      const std::vector<pending_slide>& slid)
        {
            auto pager = std::make_unique<dyld_cache_pager>(emu.memory, default_host_range_reader(), std::move(ranges));
            auto* raw = pager.get();

            std::vector<pending_slide> slide_regions{slid};

            raw->set_chunk_fixup([&emu, raw, slide_regions = std::move(slide_regions)](
                                     const uint64_t chunk_address, const std::span<std::byte> chunk_data, memory_permission) {
                const auto chunk_end = chunk_address + chunk_data.size();

                for (const auto& region : slide_regions)
                {
                    if (region.slide_size == 0 || chunk_address >= region.target + region.length || chunk_end <= region.target)
                    {
                        continue;
                    }

                    if (!emu.memory.allocate_memory(chunk_address, chunk_data.size(), memory_permission::read_write))
                    {
                        emu.log.warn("could not stage a cache chunk at 0x%" PRIx64 " for rebasing\n", chunk_address);
                        return;
                    }

                    emu.memory.write_memory(chunk_address, chunk_data.data(), chunk_data.size());

                    uint64_t applied = 0;
                    const auto rebased =
                        apply_dyld_cache_slide_info(emu, region.target, region.length, region.slide_start, applied, chunk_address,
                                                    chunk_end, [raw](const uint64_t address, const std::span<std::byte> destination) {
                                                        return raw->read_backing(address, destination);
                                                    });

                    if (rebased)
                    {
                        emu.memory.read_memory(chunk_address, chunk_data.data(), chunk_data.size());
                    }
                    else
                    {
                        emu.log.warn("could not rebase the cache chunk at 0x%" PRIx64 "\n", chunk_address);
                    }

                    (void)emu.memory.release_memory(chunk_address, chunk_data.size());
                    return;
                }
            });

            install_dyld_cache_pager(emu, std::move(pager));
        }
    }

    void sys_shared_region_map_and_slide_2_np(const macos_syscall_context& c)
    {
        const auto file_count = static_cast<uint32_t>(get_macos_syscall_argument(c, 0));
        const auto files = get_macos_syscall_argument(c, 1);
        const auto mapping_count = static_cast<uint32_t>(get_macos_syscall_argument(c, 2));
        const auto mappings = get_macos_syscall_argument(c, 3);

        if (file_count == 0 || mapping_count == 0 || files == 0 || mappings == 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // The slide info for a data region lives in the cache's read-only region, which is itself one of
        // the mappings being installed. Nothing can be walked until every mapping is in place.
        std::vector<pending_slide> slid{};
        std::vector<dyld_cache_backing_range> lazy{};

        uint32_t consumed = 0;
        uint64_t first_address = 0;
        std::string cache_path{};

        for (uint32_t index = 0; index < file_count; ++index)
        {
            shared_region_file file{};
            if (!read_shared_region_file(c, files + index * SHARED_FILE_ENTRY_SIZE, file))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            // The last descriptor in the list is -1: a reservation that holds the tail of the shared
            // region with nothing behind it, so that nothing else can be handed those addresses.
            const auto is_reservation = file.fd < 0;

            const auto* entry = is_reservation ? nullptr : c.proc.fds.get(file.fd);
            if (!is_reservation && (entry == nullptr || entry->host_path.empty()))
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            if (file.mapping_count > mapping_count - consumed)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            const std::filesystem::path host_path{is_reservation ? std::string{} : entry->host_path};

            if (cache_path.empty() && !is_reservation)
            {
                cache_path = entry->guest_path;
            }

            for (uint32_t slot = 0; slot < file.mapping_count; ++slot, ++consumed)
            {
                shared_region_mapping mapping{};
                if (!read_shared_region_mapping(c, mappings + consumed * SHARED_MAPPING_ENTRY_SIZE, mapping))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return;
                }

                // A layout that has drifted would otherwise alias arbitrary file bytes into the guest at
                // an arbitrary address. Refusing loudly is the only safe answer to a mapping that does
                // not look like one.
                if (!mapping_is_plausible(mapping))
                {
                    c.emu_ref.log.warn("shared region mapping %u of fd %d is not plausible: address=0x%" PRIx64 " size=0x%" PRIx64
                                       " prot=%d/%d\n",
                                       consumed, file.fd, mapping.address, mapping.size, mapping.init_protection, mapping.max_protection);
                    write_macos_syscall_error(c, MACOS_EINVAL);
                    return;
                }

                // sms_address is already the final address: the mappings dyld passes are contiguous and
                // match the addresses the cache was built for. sf_slide is NOT an address delta -- on
                // this host the first descriptor carries 0x10000000 there while every other carries
                // zero, and treating it as one relocates the cache header out from under the pointer
                // dyld is already holding, which it then reads as garbage.
                const auto target = mapping.address;
                const auto length = static_cast<size_t>(mapping.size);
                const auto permissions = macos_prot_to_permission(mapping.init_protection);

                // dyld mmaps every subcache to read it before asking for this, so the addresses it wants
                // are often already taken by its own scratch mappings. The kernel installs over them,
                // and so must this.
                if (c.emu_ref.memory.overlaps_mapped_region(target, length))
                {
                    c.emu_ref.memory.release_memory(target, length);
                }

                // map_host_file_range aliases the host page cache, so the host pages the mapping in
                // lazily and a 5.4 GB cache costs a few hundred megabytes of RSS. Where there is no
                // mmap to alias -- the browser -- the alternative is copying every byte into linear
                // memory, so the mapping is registered with the pager and materialised on demand
                // instead.
                const auto try_host_mapping = !is_reservation && !c.emu_ref.force_lazy_cache_paging;

                auto installed = is_reservation
                                     ? c.emu_ref.memory.allocate_memory(target, length, permissions)
                                     : (try_host_mapping &&
                                        c.emu_ref.memory.map_host_file_range(target, length, host_path, mapping.file_offset, permissions));

                if (!installed && !is_reservation)
                {
                    lazy.push_back(dyld_cache_backing_range{.address = target,
                                                            .size = length,
                                                            .path = host_path,
                                                            .file_offset = mapping.file_offset,
                                                            .permissions = permissions});
                    installed = true;
                }

                if (first_address == 0 || target < first_address)
                {
                    first_address = target;
                }

                if (installed && mapping.slide_size != 0)
                {
                    slid.push_back(
                        {.target = target, .length = length, .slide_start = mapping.slide_start, .slide_size = mapping.slide_size});
                }

                if (!installed)
                {
                    c.emu_ref.log.warn("shared region mapping %u of %s at 0x%" PRIx64 " size 0x%zx could not be installed\n", consumed,
                                       is_reservation ? "<reservation>" : entry->guest_path.c_str(), target, length);
                    write_macos_syscall_error(c, MACOS_ENOMEM);
                    return;
                }
            }
        }

        // "and_slide" is the second half of this call's name and the reason dyld can use the cache at
        // all: the packed chain entries in every data page become real pointers here, signed so the
        // guest's own autda accepts them.
        if (!lazy.empty())
        {
            install_lazy_cache_pager(c.emu_ref, std::move(lazy), slid);
        }

        uint64_t applied = 0;
        for (const auto& entry : slid)
        {
            // A lazily paged region is rebased chunk by chunk as the guest reaches it. Rebasing it here
            // would fault every page of it in, which is the whole thing the pager exists to avoid.
            if (c.emu_ref.cache_pager && c.emu_ref.cache_pager->covers(entry.target))
            {
                continue;
            }

            if (!apply_dyld_cache_slide_info(c.emu_ref, entry.target, entry.length, entry.slide_start, applied))
            {
                c.emu_ref.log.warn("could not apply the slide info at 0x%" PRIx64 " for the region at 0x%" PRIx64 "\n", entry.slide_start,
                                   entry.target);
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }
        }

        // The cache header has to be readable to find the dynamic region, and under lazy paging it is
        // not resident until something touches it. try_read_memory would report it absent and
        // publish_dynamic_region would take that for "this cache has none", which dyld then reports as
        // "mapped cache does not contain dynamic config data" before dereferencing the null it got back.
        if (first_address != 0 && c.emu_ref.cache_pager && c.emu_ref.cache_pager->covers(first_address))
        {
            (void)c.emu_ref.cache_pager->page_in(first_address);
        }

        if (first_address != 0 && !publish_dynamic_region(c, first_address, cache_path))
        {
            c.emu_ref.log.warn("could not publish the dyld dynamic region\n");
            write_macos_syscall_error(c, MACOS_ENOMEM);
            return;
        }

        c.emu_ref.log.info("shared cache: %u mappings installed, %" PRIu64 " pointers rebased\n", consumed, applied);
        c.emu_ref.callbacks.on_shared_cache_mapped(consumed, applied);

        // The first mapping's address is the base every cache-relative address is measured from, and
        // shared_region_check_np is how dyld asks for it afterwards.
        if (first_address != 0)
        {
            c.proc.shared_region_base = first_address;
        }

        // The host path of the mapped cache is stashed for subsystems that resolve cache exports after
        // this point -- the workqueue resolves _start_wqthread at spawn time, whether or not the GUI is
        // enabled.
        if (c.emu_ref.shared_cache_host_path.empty() && !cache_path.empty())
        {
            c.emu_ref.shared_cache_host_path = c.emu_ref.file_sys.translate(cache_path);
        }

        // The GUI routines are patched here and not before the run: their addresses only exist once the
        // cache is mapped, and the pages the trap is written into are these.
        if (c.emu_ref.ui.enabled && !c.emu_ref.ui.bind(c.emu_ref, c.emu_ref.file_sys.translate(cache_path)))
        {
            c.emu_ref.log.warn("GUI interception could not be installed; the guest will reach the real SkyLight stubs\n");
        }

        write_macos_syscall_result(c, 0);
    }

    // A branch-predictor hint the objc runtime asks the kernel to install for its message send path. It
    // is an optimisation with no observable semantics, and the runtime carries on without it -- which
    // is exactly what happens on hardware that does not implement the assist.
    void sys_objc_bp_assist_cfg_np(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_ENOTSUP);
    }

    void sys_getattrlist(const macos_syscall_context& c)
    {
        answer_attrlist(c, read_bounded_guest_path(c, get_macos_syscall_argument(c, 0)));
    }

    // Same encoding, named by descriptor. CoreFoundation reaches it during start-up, and the answer is
    // built from the path, so a descriptor that has none cannot be served rather than served with a guess.
    void sys_fgetattrlist(const macos_syscall_context& c)
    {
        const auto fd = static_cast<int>(static_cast<int32_t>(get_macos_syscall_argument(c, 0)));

        const auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (entry->guest_path.empty())
        {
            write_macos_syscall_error(c, MACOS_ENOTSUP);
            return;
        }

        answer_attrlist(c, entry->guest_path);
    }

    void sys_getattrlistbulk(const macos_syscall_context& c)
    {
        // Present in dyld's stub table but never executed on the measured path to main(). Reaching this
        // in a bring-up run is the signal to implement it rather than guess now.
        c.emu_ref.log.print(color::yellow, "getattrlistbulk is not implemented\n");
        write_macos_syscall_error(c, MACOS_ENOTSUP);
    }

    void bsd_syscall_dispatcher::add_dyld_handlers()
    {
        this->register_handler(MACOS_SYS_crossarch_trap, sys_crossarch_trap, "crossarch_trap");
        this->register_handler(MACOS_SYS_getattrlist, sys_getattrlist, "getattrlist");
        this->register_handler(MACOS_SYS_getfsstat64, sys_getfsstat64, "getfsstat64");
        this->register_handler(MACOS_SYS_fsgetpath, sys_fsgetpath, "fsgetpath");
        this->register_handler(MACOS_SYS_fsctl, sys_fsctl, "fsctl");
        this->register_handler(MACOS_SYS_objc_bp_assist_cfg_np, sys_objc_bp_assist_cfg_np, "objc_bp_assist_cfg_np");
        this->register_handler(MACOS_SYS_shared_region_map_and_slide_2_np, sys_shared_region_map_and_slide_2_np,
                               "shared_region_map_and_slide_2_np");
        this->register_handler(MACOS_SYS_getattrlistbulk, sys_getattrlistbulk, "getattrlistbulk");
        this->register_handler(MACOS_SYS_csrctl, sys_csrctl, "csrctl");
        this->register_handler(MACOS_SYS_mremap_encrypted, sys_mremap_encrypted, "mremap_encrypted");
        this->register_handler(MACOS_SYS_map_with_linking_np, sys_map_with_linking_np, "map_with_linking_np");
    }
}
