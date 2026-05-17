#pragma once

#include "../platform/platform.hpp"

struct compat_stat
{
    uint32_t st_dev;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    uint64_t st_ino;
    int64_t st_size;
    timespec st_atimespec;
    timespec st_mtimespec;
    timespec st_ctimespec;
};

bool compat_fstat(int fd, struct compat_stat* stat);
bool compat_stat(const char* file_name, struct compat_stat* stat);

LARGE_INTEGER convert_timespec_to_filetime(timespec timespec);
