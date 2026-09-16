#pragma once

#include "std_include.hpp"
#include "macos_platform.hpp"
#include "macos_process_context.hpp"

#include <guest/guest_fd_table.hpp>

#include <arch_emulator.hpp>
#include <hook_interface.hpp>

#include <string_view>

namespace sogen
{
    class macos_emulator;

    constexpr uint64_t MACOS_NZCV_CARRY = 0x20000000ULL;
    constexpr int32_t MACOS_PLATFORM_SYSCALL_TRAP_NO = static_cast<int32_t>(0x80000000U);
    constexpr int32_t MACOS_MACH_TRAP_ABSTIME = -3;
    constexpr int32_t MACOS_MACH_TRAP_CONTTIME = -4;
    constexpr size_t MACOS_MAX_SYSCALL_ARGUMENTS = 8;

    static_assert(static_cast<uint32_t>(arm64_register::x8) == static_cast<uint32_t>(arm64_register::x0) + 8,
                  "x0..x8 must be contiguous for register-indexed argument access");

    struct macos_syscall_context
    {
        macos_emulator& emu_ref;
        arm64_64_emulator& emu;
        macos_process_context& proc;
        size_t argument_offset{0};
    };

    using macos_syscall_handler = void (*)(const macos_syscall_context&);

    uint64_t get_macos_syscall_argument(const macos_syscall_context& c, size_t index);

    inline void clear_macos_syscall_carry(arm64_64_emulator& emu)
    {
        emu.reg(arm64_register::nzcv, emu.reg(arm64_register::nzcv) & ~MACOS_NZCV_CARRY);
    }

    inline void write_macos_syscall_result_pair(const macos_syscall_context& c, const uint64_t primary, const uint64_t secondary)
    {
        c.emu.reg(arm64_register::x0, primary);
        c.emu.reg(arm64_register::x1, secondary);
        clear_macos_syscall_carry(c.emu);
    }

    inline void write_macos_syscall_result(const macos_syscall_context& c, const int64_t result)
    {
        write_macos_syscall_result_pair(c, static_cast<uint64_t>(result), 0);
    }

    // Darwin reports errors through NZCV.C with a POSITIVE errno in x0, unlike Linux which returns
    // -errno. Passing a negated value here reaches a cerror stub that hands the caller the negative
    // number instead of setting errno, so the out-of-line definition warns rather than silently
    // accepting it.
    void write_macos_syscall_error(const macos_syscall_context& c, int64_t error);

    int64_t map_host_errno_to_macos(int host_errno);

    // A descriptor that guarded_open_np pinned a guard to may only be closed, duplicated or written
    // through the guarded_* call that names the id back. Reaching for one of those operations through
    // the ordinary syscall is what xnu answers with a fatal EXC_GUARD_FD rather than an errno, because
    // the guest asked for exactly that to be impossible and then did it anyway. Returns false having
    // stopped the run.
    bool fd_guard_permits_plain_call(const macos_syscall_context& c, int fd, uint32_t operation, std::string_view call);

    // The counterpart, for the guarded_* calls themselves. A descriptor carrying no guard is xnu's
    // EINVAL; naming the wrong id gets EPERM rather than the fatal exception, because unlike the case
    // above the caller passed a value and has somewhere to be told about it.
    bool fd_guard_matches_argument(const macos_syscall_context& c, int fd, uint64_t guard_address, std::string_view call);

    // dyld opens /dev/null to reserve descriptors 0, 1 and 2 before it will run anything, and aborts
    // with "failed to reserve stdin descriptor" if it cannot. Opening the host character device would
    // park the emulator inside a blocking read, so the guest gets an empty memory file that reads as
    // EOF and discards what is written to it, which is exactly /dev/null's contract.
    inline bool is_macos_null_device(const guest_fd& entry)
    {
        return entry.type == fd_type::memory_file && entry.guest_path == MACOS_NULL_DEVICE_PATH;
    }

#pragma pack(push, 1)

    struct macos_attrlist
    {
        uint16_t bitmapcount{};
        uint16_t reserved{};
        uint32_t commonattr{};
        uint32_t volattr{};
        uint32_t dirattr{};
        uint32_t fileattr{};
        uint32_t forkattr{};
    };

#pragma pack(pop)

    static_assert(sizeof(macos_attrlist) == 24);

    namespace macos_attr
    {
        constexpr uint32_t CMN_NAME = 0x00000001u;
        constexpr uint32_t CMN_DEVID = 0x00000002u;
        constexpr uint32_t CMN_FSID = 0x00000004u;
        constexpr uint32_t CMN_OBJTYPE = 0x00000008u;
        constexpr uint32_t CMN_OBJID = 0x00000020u;
        constexpr uint32_t CMN_PAROBJID = 0x00000080u;
        constexpr uint32_t CMN_SCRIPT = 0x00000100u;
        constexpr uint32_t CMN_CRTIME = 0x00000200u;
        constexpr uint32_t CMN_MODTIME = 0x00000400u;
        constexpr uint32_t CMN_CHGTIME = 0x00000800u;
        constexpr uint32_t CMN_ACCTIME = 0x00001000u;
        constexpr uint32_t CMN_FNDRINFO = 0x00004000u;
        constexpr uint32_t CMN_OWNERID = 0x00008000u;
        constexpr uint32_t CMN_GRPID = 0x00010000u;
        constexpr uint32_t CMN_ACCESSMASK = 0x00020000u;
        constexpr uint32_t CMN_FLAGS = 0x00040000u;
        constexpr uint32_t CMN_GEN_COUNT = 0x00080000u;
        constexpr uint32_t CMN_DOCUMENT_ID = 0x00100000u;
        constexpr uint32_t CMN_USERACCESS = 0x00200000u;
        constexpr uint32_t CMN_FILEID = 0x02000000u;
        constexpr uint32_t CMN_PARENTID = 0x04000000u;
        constexpr uint32_t CMN_FULLPATH = 0x08000000u;
        constexpr uint32_t CMN_ADDEDTIME = 0x10000000u;
        constexpr uint32_t CMN_DATA_PROTECT_FLAGS = 0x40000000u;
        constexpr uint32_t CMN_RETURNED_ATTRS = 0x80000000u;
        constexpr uint32_t CMN_SUPPORTED = CMN_RETURNED_ATTRS | CMN_NAME | CMN_DEVID | CMN_FSID | CMN_OBJTYPE | CMN_OBJID | CMN_PAROBJID |
                                           CMN_SCRIPT | CMN_CRTIME | CMN_MODTIME | CMN_CHGTIME | CMN_ACCTIME | CMN_FNDRINFO | CMN_OWNERID |
                                           CMN_GRPID | CMN_ACCESSMASK | CMN_FLAGS | CMN_GEN_COUNT | CMN_DOCUMENT_ID | CMN_USERACCESS |
                                           CMN_FILEID | CMN_PARENTID | CMN_FULLPATH | CMN_ADDEDTIME | CMN_DATA_PROTECT_FLAGS;

        // The volume group. ATTR_VOL_INFO carries no data of its own; it marks the rest of the word as
        // volume attributes. CoreFoundation asks for these while deciding how to compare path names, and
        // refusing them aborts it.
        constexpr uint32_t VOL_FSTYPE = 0x00000001u;
        constexpr uint32_t VOL_SIGNATURE = 0x00000002u;
        constexpr uint32_t VOL_SIZE = 0x00000004u;
        constexpr uint32_t VOL_SPACEFREE = 0x00000008u;
        constexpr uint32_t VOL_SPACEAVAIL = 0x00000010u;
        constexpr uint32_t VOL_MINALLOCATION = 0x00000020u;
        constexpr uint32_t VOL_ALLOCATIONCLUMP = 0x00000040u;
        constexpr uint32_t VOL_IOBLOCKSIZE = 0x00000080u;
        constexpr uint32_t VOL_OBJCOUNT = 0x00000100u;
        constexpr uint32_t VOL_FILECOUNT = 0x00000200u;
        constexpr uint32_t VOL_DIRCOUNT = 0x00000400u;
        constexpr uint32_t VOL_MAXOBJCOUNT = 0x00000800u;
        constexpr uint32_t VOL_MOUNTPOINT = 0x00001000u;
        constexpr uint32_t VOL_NAME = 0x00002000u;
        constexpr uint32_t VOL_MOUNTFLAGS = 0x00004000u;
        constexpr uint32_t VOL_MOUNTEDDEVICE = 0x00008000u;
        constexpr uint32_t VOL_ENCODINGSUSED = 0x00010000u;
        constexpr uint32_t VOL_CAPABILITIES = 0x00020000u;
        constexpr uint32_t VOL_UUID = 0x00040000u;
        constexpr uint32_t VOL_QUOTA_SIZE = 0x10000000u;
        constexpr uint32_t VOL_RESERVED_SIZE = 0x20000000u;
        constexpr uint32_t VOL_ATTRIBUTES = 0x40000000u;
        constexpr uint32_t VOL_INFO = 0x80000000u;
        constexpr uint32_t VOL_SUPPORTED = VOL_FSTYPE | VOL_SIGNATURE | VOL_SIZE | VOL_SPACEFREE | VOL_SPACEAVAIL | VOL_MINALLOCATION |
                                           VOL_ALLOCATIONCLUMP | VOL_IOBLOCKSIZE | VOL_OBJCOUNT | VOL_FILECOUNT | VOL_DIRCOUNT |
                                           VOL_MAXOBJCOUNT | VOL_MOUNTPOINT | VOL_NAME | VOL_MOUNTFLAGS | VOL_MOUNTEDDEVICE |
                                           VOL_ENCODINGSUSED | VOL_CAPABILITIES | VOL_UUID | VOL_QUOTA_SIZE | VOL_RESERVED_SIZE |
                                           VOL_ATTRIBUTES | VOL_INFO;

        // The directory and file groups follow the volume group; the kernel emits only the one that
        // matches the object's type.
        constexpr uint32_t DIR_LINKCOUNT = 0x00000001u;
        constexpr uint32_t DIR_ENTRYCOUNT = 0x00000002u;
        constexpr uint32_t DIR_MOUNTSTATUS = 0x00000004u;
        constexpr uint32_t DIR_ALLOCSIZE = 0x00000008u;
        constexpr uint32_t DIR_IOBLOCKSIZE = 0x00000010u;
        constexpr uint32_t DIR_DATALENGTH = 0x00000020u;
        constexpr uint32_t DIR_SUPPORTED =
            DIR_LINKCOUNT | DIR_ENTRYCOUNT | DIR_MOUNTSTATUS | DIR_ALLOCSIZE | DIR_IOBLOCKSIZE | DIR_DATALENGTH;

        constexpr uint32_t FILE_LINKCOUNT = 0x00000001u;
        constexpr uint32_t FILE_TOTALSIZE = 0x00000002u;
        constexpr uint32_t FILE_ALLOCSIZE = 0x00000004u;
        constexpr uint32_t FILE_IOBLOCKSIZE = 0x00000008u;
        constexpr uint32_t FILE_DEVTYPE = 0x00000020u;
        constexpr uint32_t FILE_DATALENGTH = 0x00000200u;
        constexpr uint32_t FILE_DATAALLOCSIZE = 0x00000400u;
        constexpr uint32_t FILE_RSRCLENGTH = 0x00001000u;
        constexpr uint32_t FILE_RSRCALLOCSIZE = 0x00002000u;
        constexpr uint32_t FILE_SUPPORTED = FILE_LINKCOUNT | FILE_TOTALSIZE | FILE_ALLOCSIZE | FILE_IOBLOCKSIZE | FILE_DEVTYPE |
                                            FILE_DATALENGTH | FILE_DATAALLOCSIZE | FILE_RSRCLENGTH | FILE_RSRCALLOCSIZE;

        // The fork group carries no fork attributes on any modern volume: xnu reuses the word for the
        // ATTR_CMNEXT_* set, which is only interpreted when FSOPT_ATTR_CMN_EXTENDED is in the options.
        constexpr uint32_t CMNEXT_RELPATH = 0x00000004u;
        constexpr uint32_t CMNEXT_PRIVATESIZE = 0x00000008u;
        constexpr uint32_t CMNEXT_LINKID = 0x00000010u;
        constexpr uint32_t CMNEXT_NOFIRMLINKPATH = 0x00000020u;
        constexpr uint32_t CMNEXT_REALDEVID = 0x00000040u;
        constexpr uint32_t CMNEXT_REALFSID = 0x00000080u;
        constexpr uint32_t CMNEXT_CLONEID = 0x00000100u;
        constexpr uint32_t CMNEXT_EXT_FLAGS = 0x00000200u;
        constexpr uint32_t CMNEXT_RECURSIVE_GENCOUNT = 0x00000400u;
        constexpr uint32_t CMNEXT_SUPPORTED = CMNEXT_RELPATH | CMNEXT_PRIVATESIZE | CMNEXT_LINKID | CMNEXT_NOFIRMLINKPATH |
                                              CMNEXT_REALDEVID | CMNEXT_REALFSID | CMNEXT_CLONEID | CMNEXT_EXT_FLAGS |
                                              CMNEXT_RECURSIVE_GENCOUNT;

        constexpr uint32_t OBJ_TYPE_VREG = 1;
        constexpr uint32_t OBJ_TYPE_VDIR = 2;
        constexpr uint32_t OBJ_TYPE_VLNK = 5;
    }

    void sys_crossarch_trap(const macos_syscall_context& c);
    void sys_getattrlist(const macos_syscall_context& c);
    void sys_fgetattrlist(const macos_syscall_context& c);
    void sys_getattrlistbulk(const macos_syscall_context& c);
    void sys_fsctl(const macos_syscall_context& c);
    void sys_kdebug_trace(const macos_syscall_context& c);
    void sys_kdebug_trace_string(const macos_syscall_context& c);
    void sys_kdebug_typefilter(const macos_syscall_context& c);
    void sys_pathconf(const macos_syscall_context& c);
    void sys_fpathconf(const macos_syscall_context& c);
    void sys_objc_bp_assist_cfg_np(const macos_syscall_context& c);
    void sys_shared_region_map_and_slide_2_np(const macos_syscall_context& c);
    void sys_getfsstat64(const macos_syscall_context& c);
    void sys_fsgetpath(const macos_syscall_context& c);
    void sys_csrctl(const macos_syscall_context& c);
    void sys_mremap_encrypted(const macos_syscall_context& c);
    void sys_map_with_linking_np(const macos_syscall_context& c);
    void sys_platform_syscall(const macos_syscall_context& c);
    void sys_getrlimit(const macos_syscall_context& c);
    void sys_setrlimit(const macos_syscall_context& c);
    void sys_getaudit_addr(const macos_syscall_context& c);
    void sys_gettid(const macos_syscall_context& c);
    void sys_sigaction(const macos_syscall_context& c);

}
