#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif

#ifndef SYS_openat
#define SYS_openat 257
#endif

#ifndef SYS_newfstatat
#define SYS_newfstatat 262
#endif

#ifndef SYS_unlinkat
#define SYS_unlinkat 263
#endif

#ifndef SYS_faccessat
#define SYS_faccessat 269
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

struct linux_dirent64
{
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

#define CHECK(cond, msg)                                          \
    do                                                            \
    {                                                             \
        if (!(cond))                                              \
        {                                                         \
            fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno); \
            return 1;                                             \
        }                                                         \
    } while (0)

static int contains_name(const char names[][256], int count, const char* needle)
{
    for (int i = 0; i < count; ++i)
    {
        if (strcmp(names[i], needle) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int linux_openat(int dirfd, const char* pathname, int flags, mode_t mode)
{
    return (int)syscall(SYS_openat, dirfd, pathname, flags, mode);
}

static int linux_newfstatat(int dirfd, const char* pathname, struct stat* st, int flags)
{
    return (int)syscall(SYS_newfstatat, dirfd, pathname, st, flags);
}

static int linux_faccessat(int dirfd, const char* pathname, int mode, int flags)
{
    return (int)syscall(SYS_faccessat, dirfd, pathname, mode, flags);
}

static int linux_unlinkat(int dirfd, const char* pathname, int flags)
{
    return (int)syscall(SYS_unlinkat, dirfd, pathname, flags);
}

int main(void)
{
    char dir_template[] = "/tmp/sogen_openat_tortureXXXXXX";
    char* dir_path = mkdtemp(dir_template);
    CHECK(dir_path != NULL, "mkdtemp");

    char file_a[512];
    char file_b[512];
    snprintf(file_a, sizeof(file_a), "%s/%s", dir_path, "a.txt");
    snprintf(file_b, sizeof(file_b), "%s/%s", dir_path, "b.txt");

    const int fd_a = linux_openat(AT_FDCWD, file_a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd_a >= 0, "openat create a.txt");
    CHECK(write(fd_a, "abc", 3) == 3, "write a.txt");

    const int fd_b = linux_openat(AT_FDCWD, file_b, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd_b >= 0, "openat create b.txt");
    CHECK(write(fd_b, "xyz", 3) == 3, "write b.txt");

    CHECK(fsync(fd_a) == 0, "fsync a.txt");
    CHECK(fsync(fd_b) == 0, "fsync b.txt");

    struct stat st_empty = {0};
    CHECK(linux_newfstatat(fd_a, "", &st_empty, AT_EMPTY_PATH) == 0, "newfstatat AT_EMPTY_PATH");
    CHECK((int)st_empty.st_size == 3, "newfstatat size check");

    CHECK(linux_faccessat(AT_FDCWD, file_a, R_OK | W_OK, 0) == 0, "faccessat R_OK|W_OK");

    const int dfd = linux_openat(AT_FDCWD, dir_path, O_RDONLY | O_DIRECTORY, 0);
    CHECK(dfd >= 0, "openat AT_FDCWD dir");

    char dent_buf[512];
    char names[64][256];
    int name_count = 0;

    while (1)
    {
        const int nread = syscall(SYS_getdents64, dfd, dent_buf, sizeof(dent_buf));
        CHECK(nread >= 0, "getdents64");

        if (nread == 0)
        {
            break;
        }

        int bpos = 0;
        while (bpos < nread)
        {
            struct linux_dirent64* d = (struct linux_dirent64*)(dent_buf + bpos);
            if (name_count < 64)
            {
                strncpy(names[name_count], d->d_name, sizeof(names[name_count]) - 1);
                names[name_count][sizeof(names[name_count]) - 1] = '\0';
                ++name_count;
            }
            bpos += d->d_reclen;
        }
    }

    CHECK(contains_name(names, name_count, "."), "getdents64 includes .");
    CHECK(contains_name(names, name_count, ".."), "getdents64 includes ..");
    CHECK(contains_name(names, name_count, "a.txt"), "getdents64 includes a.txt");
    CHECK(contains_name(names, name_count, "b.txt"), "getdents64 includes b.txt");

    CHECK(close(dfd) == 0, "close directory fd");
    CHECK(close(fd_a) == 0, "close fd_a");
    CHECK(close(fd_b) == 0, "close fd_b");

    CHECK(linux_unlinkat(AT_FDCWD, file_a, 0) == 0, "unlinkat a.txt");
    CHECK(linux_unlinkat(AT_FDCWD, file_b, 0) == 0, "unlinkat b.txt");
    CHECK(rmdir(dir_path) == 0, "rmdir temp directory");

    puts("ALL PASS openat_getdents_torture");
    return 0;
}
