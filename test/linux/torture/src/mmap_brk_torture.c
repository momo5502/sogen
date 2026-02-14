#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifndef SYS_brk
#define SYS_brk 12
#endif

#ifndef SYS_mprotect
#define SYS_mprotect 10
#endif

#ifndef SYS_munmap
#define SYS_munmap 11
#endif

#define CHECK(cond, msg)                                          \
    do                                                            \
    {                                                             \
        if (!(cond))                                              \
        {                                                         \
            fprintf(stderr, "FAIL: %s (errno=%d)\n", msg, errno); \
            return 1;                                             \
        }                                                         \
    } while (0)

static uintptr_t linux_brk(uintptr_t addr)
{
    return (uintptr_t)syscall(SYS_brk, addr);
}

int main(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096, "page size sanity");

    void* map1 = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map1 != MAP_FAILED, "initial mmap");

    errno = 0;
    void* noreplace_same = mmap(map1, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(noreplace_same == MAP_FAILED && errno == EEXIST, "MAP_FIXED_NOREPLACE same address overlap");

    errno = 0;
    void* noreplace_partial =
        mmap((char*)map1 + page, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(noreplace_partial == MAP_FAILED && errno == EEXIST, "MAP_FIXED_NOREPLACE partial overlap");

    void* map_unaligned = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map_unaligned != MAP_FAILED, "mmap for unaligned checks");

    errno = 0;
    const long munmap_rc = syscall(SYS_munmap, (char*)map_unaligned + 1, page);
    CHECK(munmap_rc == 0 || (munmap_rc == -1 && (errno == EINVAL || errno == ENOMEM)), "munmap unaligned behavior is stable");

    errno = 0;
    const long mprotect_rc = syscall(SYS_mprotect, (char*)map_unaligned + 1, page, PROT_READ);
    CHECK(mprotect_rc == 0 || (mprotect_rc == -1 && (errno == EINVAL || errno == ENOMEM)), "mprotect unaligned behavior is stable");

    if (munmap_rc != 0)
    {
        (void)munmap(map_unaligned, page * 2);
    }

    void* frag = mmap(NULL, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(frag != MAP_FAILED, "fragmentation mmap");

    const uintptr_t brk0 = linux_brk(0);
    CHECK(brk0 != 0, "brk query");

    const uintptr_t grow_target = (brk0 + page * 3 + (page - 1)) & ~(uintptr_t)(page - 1);
    const uintptr_t brk1 = linux_brk(grow_target);
    CHECK(brk1 >= grow_target, "brk grow");

    const uintptr_t brk2 = linux_brk(brk0);
    CHECK(brk2 <= brk1, "brk shrink");

    const uintptr_t brk3 = linux_brk(grow_target);
    CHECK(brk3 >= grow_target, "brk regrow");

    CHECK(munmap(frag, page * 4) == 0, "munmap fragmentation mapping");
    CHECK(munmap(map1, page * 2) == 0, "munmap initial mapping");

    puts("ALL PASS mmap_brk_torture");
    return 0;
}
