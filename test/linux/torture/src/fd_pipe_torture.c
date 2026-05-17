#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pipe2
#define SYS_pipe2 293
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

int main(void)
{
    char path[] = "/tmp/sogen_fd_pipe_tortureXXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");

    /* Keep only the file descriptor; remove directory entry to avoid leftovers. */
    CHECK(unlink(path) == 0, "unlink temp file");

    const char payload[] = "ABCDEFGHIJ";
    CHECK(write(fd, payload, sizeof(payload) - 1) == (ssize_t)(sizeof(payload) - 1), "write initial payload");
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek rewind");

    const int dupfd = dup(fd);
    CHECK(dupfd >= 0, "dup");

    char buf[64] = {0};
    CHECK(read(dupfd, buf, 4) == 4, "read from dupfd");
    CHECK(memcmp(buf, "ABCD", 4) == 0, "dup read payload check");

    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, 3) == 3, "read from original after dup read");
    CHECK(memcmp(buf, "EFG", 3) == 0, "shared file offset across dup descriptors");

    int fdflags = fcntl(dupfd, F_GETFD);
    CHECK(fdflags >= 0, "fcntl(F_GETFD)");
    CHECK(fcntl(dupfd, F_SETFD, fdflags | FD_CLOEXEC) == 0, "fcntl(F_SETFD, FD_CLOEXEC)");
    fdflags = fcntl(dupfd, F_GETFD);
    CHECK(fdflags >= 0 && (fdflags & FD_CLOEXEC) != 0, "FD_CLOEXEC persisted");

    errno = 0;
    CHECK(read(-1, buf, 1) == -1 && errno == EBADF, "invalid fd returns EBADF");

    int pipefd[2] = {-1, -1};
    CHECK(syscall(SYS_pipe2, pipefd, 0) == 0, "pipe2");

    const char msg[] = "pipe-message";
    CHECK(write(pipefd[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg), "pipe write");

    memset(buf, 0, sizeof(buf));
    CHECK(read(pipefd[0], buf, sizeof(msg)) == (ssize_t)sizeof(msg), "pipe read");
    CHECK(memcmp(buf, msg, sizeof(msg)) == 0, "pipe roundtrip integrity");

    CHECK(close(pipefd[1]) == 0, "close pipe write end");
    CHECK(read(pipefd[0], buf, sizeof(buf)) == 0, "pipe EOF after writer close");
    CHECK(close(pipefd[0]) == 0, "close pipe read end");

    int nbpipe[2] = {-1, -1};
    CHECK(syscall(SYS_pipe2, nbpipe, O_NONBLOCK) == 0, "pipe2 O_NONBLOCK");
    errno = 0;
    CHECK(read(nbpipe[0], buf, sizeof(buf)) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK), "nonblocking empty read");
    CHECK(close(nbpipe[0]) == 0, "close nonblocking read end");
    CHECK(close(nbpipe[1]) == 0, "close nonblocking write end");

    CHECK(close(dupfd) == 0, "close dupfd");
    CHECK(close(fd) == 0, "close fd");

    puts("ALL PASS fd_pipe_torture");
    return 0;
}
