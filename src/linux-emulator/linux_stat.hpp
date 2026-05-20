#pragma once

#include "std_include.hpp"

// Linux x86-64 struct stat layout (144 bytes)
// Matches the kernel's struct stat for x86-64
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
#pragma pack(push, 1)
namespace sogen
{

    struct linux_stat
    {
        uint64_t st_dev;
        uint64_t st_ino;
        uint64_t st_nlink;
        uint32_t st_mode;
        uint32_t st_uid;
        uint32_t st_gid;
        uint32_t pad0_;
        uint64_t st_rdev;
        int64_t st_size;
        int64_t st_blksize;
        int64_t st_blocks;
        uint64_t st_atime_sec;
        uint64_t st_atime_nsecs;
        uint64_t st_mtime_sec;
        uint64_t st_mtime_nsecs;
        uint64_t st_ctime_sec;
        uint64_t st_ctime_nsecs;
        int64_t reserved_[3];
    };
#pragma pack(pop)
    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

    static_assert(sizeof(linux_stat) == 144);

} // namespace sogen
