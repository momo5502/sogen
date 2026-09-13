#pragma once

#include "macos_platform.hpp"

#include <utils/stat.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sogen
{
    constexpr size_t MACOS_FSTYPENAME_LENGTH = 16;

#pragma pack(push, 1)

    struct macos_timespec
    {
        int64_t tv_sec{};
        int64_t tv_nsec{};
    };

    struct macos_stat64
    {
        int32_t st_dev{};
        uint16_t st_mode{};
        uint16_t st_nlink{};
        uint64_t st_ino{};
        uint32_t st_uid{};
        uint32_t st_gid{};
        int32_t st_rdev{};
        // Darwin's struct stat64 is not a packed structure: struct timespec is eight byte aligned, so the
        // compiler leaves four bytes of padding here. The whole layout is spelled out by hand because the
        // CI legs also build for Linux and Windows, whose own struct stat disagrees with Darwin's.
        uint32_t st_timespec_padding{};
        macos_timespec st_atimespec{};
        macos_timespec st_mtimespec{};
        macos_timespec st_ctimespec{};
        macos_timespec st_birthtimespec{};
        int64_t st_size{};
        int64_t st_blocks{};
        int32_t st_blksize{};
        uint32_t st_flags{};
        uint32_t st_gen{};
        int32_t st_lspare{};
        std::array<int64_t, 2> st_qspare{};
    };

    struct macos_statfs64
    {
        uint32_t f_bsize{};
        int32_t f_iosize{};
        uint64_t f_blocks{};
        uint64_t f_bfree{};
        uint64_t f_bavail{};
        uint64_t f_files{};
        uint64_t f_ffree{};
        std::array<int32_t, 2> f_fsid{};
        uint32_t f_owner{};
        uint32_t f_type{};
        uint32_t f_flags{};
        uint32_t f_fssubtype{};
        std::array<char, MACOS_FSTYPENAME_LENGTH> f_fstypename{};
        std::array<char, MACOS_PATH_MAX> f_mntonname{};
        std::array<char, MACOS_PATH_MAX> f_mntfromname{};
        uint32_t f_flags_ext{};
        std::array<uint32_t, 7> f_reserved{};
    };

    struct macos_dirent64_header
    {
        uint64_t d_ino{};
        uint64_t d_seekoff{};
        uint16_t d_reclen{};
        uint16_t d_namlen{};
        uint8_t d_type{};
    };

#pragma pack(pop)

    static_assert(sizeof(macos_stat64) == 144);
    static_assert(offsetof(macos_stat64, st_ino) == 8);
    static_assert(offsetof(macos_stat64, st_size) == 96);
    static_assert(offsetof(macos_stat64, st_blksize) == 112);
    static_assert(offsetof(macos_stat64, st_birthtimespec) == 80);

    static_assert(sizeof(macos_statfs64) == 2168);
    static_assert(offsetof(macos_statfs64, f_fstypename) == 72);
    static_assert(offsetof(macos_statfs64, f_mntonname) == 88);
    static_assert(offsetof(macos_statfs64, f_mntfromname) == 1112);

    static_assert(sizeof(macos_dirent64_header) == 21);

    // xnu's DIRENT64_LEN measures from sizeof(struct direntry) - MAXPATHLEN, which is 24 rather than the
    // 21 bytes the header occupies: the structure carries uint64_t members and so rounds up to eight.
    // Records built from 21 come out eight bytes short of what libc's readdir walks over.
    constexpr size_t MACOS_DIRENT_RECORD_BASE = 24;
    constexpr size_t MACOS_DIRENT_ALIGNMENT = 8;
    constexpr size_t MACOS_DIRENT_MAX_RECORD =
        ((MACOS_DIRENT_RECORD_BASE + MACOS_PATH_MAX + 1) + (MACOS_DIRENT_ALIGNMENT - 1)) & ~(MACOS_DIRENT_ALIGNMENT - 1);

    macos_stat64 host_stat_to_macos(const struct compat_stat& host, uint32_t uid, uint32_t gid);
}
