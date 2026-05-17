#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_rt_sigaction
#define SYS_rt_sigaction 13
#endif

#ifndef SYS_rt_sigprocmask
#define SYS_rt_sigprocmask 14
#endif

#ifndef SYS_futex
#define SYS_futex 202
#endif

/* Linux x86-64 kernel sigaction layout. */
struct linux_kernel_sigaction
{
    uint64_t linux_sa_handler;
    uint64_t linux_sa_flags;
    uint64_t linux_sa_restorer;
    uint64_t linux_sa_mask;
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

/* Futex operations. */
enum
{
    LINUX_FUTEX_WAIT = 0,
    LINUX_FUTEX_WAKE = 1,
    LINUX_FUTEX_WAIT_BITSET = 9,
    LINUX_FUTEX_WAKE_BITSET = 10,
    LINUX_FUTEX_PRIVATE_FLAG = 128,
    LINUX_FUTEX_BITSET_MATCH_ANY = -1
};

struct linux_timespec
{
    int64_t tv_sec;
    int64_t tv_nsec;
};

static void dummy_handler(int signo)
{
    (void)signo;
}

int main(void)
{
    struct linux_kernel_sigaction act;
    struct linux_kernel_sigaction old_act;
    memset(&act, 0, sizeof(act));
    memset(&old_act, 0, sizeof(old_act));

    act.linux_sa_handler = (uint64_t)(uintptr_t)dummy_handler;
    act.linux_sa_flags = 0;
    act.linux_sa_restorer = 0;
    act.linux_sa_mask = 0;

    CHECK(syscall(SYS_rt_sigaction, SIGUSR1, &act, &old_act, 8) == 0, "rt_sigaction set handler");

    struct linux_kernel_sigaction queried;
    memset(&queried, 0, sizeof(queried));
    CHECK(syscall(SYS_rt_sigaction, SIGUSR1, NULL, &queried, 8) == 0, "rt_sigaction query handler");
    CHECK(queried.linux_sa_handler == (uint64_t)(uintptr_t)dummy_handler, "rt_sigaction roundtrip handler pointer");

    uint64_t old_mask = 0;
    const uint64_t block_mask = (uint64_t)1 << (SIGUSR1 - 1);

    CHECK(syscall(SYS_rt_sigprocmask, SIG_BLOCK, &block_mask, &old_mask, 8) == 0, "rt_sigprocmask block");
    CHECK(syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &block_mask, &old_mask, 8) == 0, "rt_sigprocmask unblock");

    uint32_t fut = 123;

    /* Value mismatch path should return EAGAIN. */
    errno = 0;
    CHECK(syscall(SYS_futex, &fut, LINUX_FUTEX_WAIT | LINUX_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, 0) == -1 && errno == EAGAIN,
          "futex wait value mismatch");

    /* Deterministic no-wait path: zero timeout avoids indefinite block on native Linux. */
    struct linux_timespec zero_timeout;
    zero_timeout.tv_sec = 0;
    zero_timeout.tv_nsec = 0;

    errno = 0;
    CHECK(syscall(SYS_futex, &fut, LINUX_FUTEX_WAIT | LINUX_FUTEX_PRIVATE_FLAG, 123, &zero_timeout, NULL, 0) == -1 &&
              (errno == EAGAIN || errno == ETIMEDOUT),
          "futex wait deterministic timeout/fallback");

    CHECK(syscall(SYS_futex, &fut, LINUX_FUTEX_WAKE | LINUX_FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0) == 0, "futex wake with no waiters");

    errno = 0;
    CHECK(syscall(SYS_futex, &fut, LINUX_FUTEX_WAIT_BITSET | LINUX_FUTEX_PRIVATE_FLAG, 0, NULL, NULL, LINUX_FUTEX_BITSET_MATCH_ANY) == -1 &&
              errno == EAGAIN,
          "futex wait_bitset mismatch");

    CHECK(syscall(SYS_futex, &fut, LINUX_FUTEX_WAKE_BITSET | LINUX_FUTEX_PRIVATE_FLAG, 1, NULL, NULL, LINUX_FUTEX_BITSET_MATCH_ANY) == 0,
          "futex wake_bitset with no waiters");

    puts("ALL PASS signals_futex_torture");
    return 0;
}
