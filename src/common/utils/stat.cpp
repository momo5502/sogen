#define _FILE_OFFSET_BITS 64

#include "stat.hpp"
#include "time.hpp"

#if defined(_WIN32) && !defined(__MINGW64__)
#include <corecrt_io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <sys/stat.h>

#undef st_atime
#undef st_mtime
#undef st_ctime

LARGE_INTEGER convert_timespec_to_filetime(const timespec timespec)
{
    return {.QuadPart = (timespec.tv_sec * HUNDRED_NANOSECONDS_IN_ONE_SECOND) + timespec.tv_nsec + WINDOWS_EPOCH_DIFFERENCE};
}

#if defined(_WIN32) && !defined(__MINGW64__)

static timespec convert_filetime_to_timespec(const FILETIME windows_time, timespec fallback_time = {.tv_sec = 0, .tv_nsec = 0})
{
    if (windows_time.dwHighDateTime == 0 && windows_time.dwLowDateTime == 0)
    {
        return fallback_time;
    }

    const LARGE_INTEGER time{.LowPart = windows_time.dwLowDateTime, .HighPart = static_cast<LONG>(windows_time.dwHighDateTime)};
    const auto value = time.QuadPart - WINDOWS_EPOCH_DIFFERENCE;
    return {.tv_sec = value / HUNDRED_NANOSECONDS_IN_ONE_SECOND, .tv_nsec = static_cast<LONG>(value % HUNDRED_NANOSECONDS_IN_ONE_SECOND)};
}

static bool do_stat(HANDLE handle, struct compat_stat* stat)
{
    const auto file_type = GetFileType(handle);
    if (file_type != FILE_TYPE_DISK)
    {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!GetFileInformationByHandle(handle, &file_info))
    {
        return false;
    }

    stat->st_dev = 0;
    stat->st_rdev = 0;
    stat->st_uid = 0;
    stat->st_gid = 0;
    stat->st_nlink = 1;

    auto st_mode = (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR | _S_IEXEC : _S_IFREG;
    st_mode |= (file_info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? _S_IREAD : _S_IREAD | _S_IWRITE;
    st_mode |= (st_mode & 0700) >> 3;
    st_mode |= (st_mode & 0700) >> 6;
    stat->st_mode = st_mode;

    stat->st_mtimespec = convert_filetime_to_timespec(file_info.ftLastWriteTime);
    stat->st_atimespec = convert_filetime_to_timespec(file_info.ftLastAccessTime, stat->st_mtimespec);
    stat->st_ctimespec = convert_filetime_to_timespec(file_info.ftCreationTime, stat->st_mtimespec);

    stat->st_size = static_cast<int64_t>((file_info.nFileSizeHigh * 0x100000000ULL) + file_info.nFileSizeLow);
    stat->st_ino = (file_info.nFileIndexHigh * 0x100000000ULL) + file_info.nFileIndexLow;

    return true;
}

bool compat_fstat(int fd, struct compat_stat* stat)
{
    auto* const handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    return do_stat(handle, stat);
}

bool compat_stat(const char* file_name, struct compat_stat* stat)
{
    if (file_name == nullptr)
    {
        return false;
    }

    auto* const file_handle = CreateFileA(file_name, FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (file_handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const auto result = do_stat(file_handle, stat);
    CloseHandle(file_handle);

    return result;
}

#else

#if defined(__MINGW64__)

static constexpr timespec time64_to_timespec(int64_t time64)
{
    const auto value = (time64 * HUNDRED_NANOSECONDS_IN_ONE_SECOND) + WINDOWS_EPOCH_DIFFERENCE;
    return {.tv_sec = value / HUNDRED_NANOSECONDS_IN_ONE_SECOND, .tv_nsec = static_cast<LONG>(value % HUNDRED_NANOSECONDS_IN_ONE_SECOND)};
}

#endif

static void set_stat(struct compat_stat* dst, const struct stat* src)
{
    dst->st_dev = 0;
    dst->st_rdev = 0;
    dst->st_uid = 0;
    dst->st_gid = 0;
    dst->st_nlink = 1;
    dst->st_mode = src->st_mode & (S_IFMT | 0700);
    dst->st_mode |= (src->st_mode & 0700) >> 3;
    dst->st_mode |= (src->st_mode & 0700) >> 6;
    dst->st_ino = src->st_ino;
    dst->st_size = src->st_size;
#ifdef OS_MAC
    dst->st_atimespec = src->st_atimespec;
    dst->st_mtimespec = src->st_mtimespec;
    dst->st_ctimespec = src->st_ctimespec;
#elif defined(__MINGW64__)
    dst->st_atimespec = time64_to_timespec(src->st_atime);
    dst->st_mtimespec = time64_to_timespec(src->st_mtime);
    dst->st_ctimespec = time64_to_timespec(src->st_ctime);
#else
    dst->st_atimespec = src->st_atim;
    dst->st_mtimespec = src->st_mtim;
    dst->st_ctimespec = src->st_ctim;
#endif
}

bool compat_fstat(int fd, struct compat_stat* stat)
{
    struct stat file_stat{};
    if (fstat(fd, &file_stat))
    {
        return false;
    }

    set_stat(stat, &file_stat);

    return true;
}

bool compat_stat(const char* file_name, struct compat_stat* stat)
{
    struct stat file_stat{};
    if (::stat(file_name, &file_stat) != 0)
    {
        return false;
    }

    set_stat(stat, &file_stat);

    return true;
}

#endif
