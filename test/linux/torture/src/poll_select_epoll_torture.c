#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_poll
#define SYS_poll 7
#endif

#ifndef SYS_select
#define SYS_select 23
#endif

#ifndef SYS_epoll_create1
#define SYS_epoll_create1 291
#endif

#ifndef SYS_epoll_ctl
#define SYS_epoll_ctl 233
#endif

#ifndef SYS_epoll_wait
#define SYS_epoll_wait 232
#endif

enum
{
    LINUX_POLLIN = 0x0001,
    LINUX_POLLOUT = 0x0004,
    LINUX_POLLNVAL = 0x0020,

    LINUX_EPOLLIN = 0x001,
    LINUX_EPOLLOUT = 0x004,

    LINUX_EPOLL_CTL_ADD = 1
};

struct linux_pollfd
{
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct linux_epoll_event
{
    uint32_t events;
    uint64_t data;
};

struct linux_timeval
{
    int64_t tv_sec;
    int64_t tv_usec;
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

int main(void)
{
    char path[] = "/tmp/sogen_poll_epoll_tortureXXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");

    CHECK(unlink(path) == 0, "unlink temp file");

    const char payload[] = "poll-select-epoll";
    CHECK(write(fd, payload, sizeof(payload) - 1) == (ssize_t)(sizeof(payload) - 1), "write payload");
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "rewind payload");

    struct linux_pollfd pfds[2];
    memset(pfds, 0, sizeof(pfds));
    pfds[0].fd = fd;
    pfds[0].events = (int16_t)(LINUX_POLLIN | LINUX_POLLOUT);
    pfds[1].fd = -1;
    pfds[1].events = (int16_t)LINUX_POLLIN;

    const int poll_rc = (int)syscall(SYS_poll, pfds, 2, 0);
    CHECK(poll_rc >= 1, "poll ready count");
    CHECK((pfds[0].revents & LINUX_POLLOUT) != 0, "poll reports writable");
    CHECK((pfds[0].revents & LINUX_POLLIN) != 0, "poll reports readable");
    CHECK((pfds[1].revents & LINUX_POLLNVAL) == 0, "poll ignores negative fd entry");

    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_SET(fd, &read_set);
    FD_SET(fd, &write_set);

    struct linux_timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    const int select_rc = (int)syscall(SYS_select, fd + 1, &read_set, &write_set, NULL, &tv);
    CHECK(select_rc >= 1, "select ready count");
    CHECK(FD_ISSET(fd, &write_set), "select reports writable");
    CHECK(FD_ISSET(fd, &read_set), "select reports readable");

    const int epfd = (int)syscall(SYS_epoll_create1, 0);
    CHECK(epfd >= 0, "epoll_create1");

    struct linux_epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = LINUX_EPOLLIN | LINUX_EPOLLOUT;
    ev.data = 0x1234;
    CHECK(syscall(SYS_epoll_ctl, epfd, LINUX_EPOLL_CTL_ADD, fd, &ev) == 0, "epoll_ctl add");

    struct linux_epoll_event out[4];
    memset(out, 0, sizeof(out));
    const int ep_rc = (int)syscall(SYS_epoll_wait, epfd, out, 4, 0);
    CHECK(ep_rc >= 1, "epoll_wait ready count");
    CHECK((out[0].events & LINUX_EPOLLOUT) != 0, "epoll reports writable");

    CHECK(close(epfd) == 0, "close epoll fd");
    CHECK(close(fd) == 0, "close data fd");

    puts("ALL PASS poll_select_epoll_torture");
    return 0;
}
