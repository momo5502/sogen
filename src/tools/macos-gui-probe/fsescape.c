// Measures whether the emulator's filesystem containment actually holds.
//
// Every case names a guest path and an operation, prints one FSESCAPE line, and never aborts: the
// point is the full table, not the first failure. Run it against a root staged by
// src/tools/make-macos-root.sh, with the canaries below planted on the host first.
//
//   clang -arch arm64 -O2 -o /tmp/fsescape fsescape.c
//   analyzer --os=macos -s -e /tmp/sogen-macos-root /tmp/fsescape

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report(const char* op, const char* path, int ok, int err, const char* detail)
{
    printf("FSESCAPE %-6s %-8s %-52s %s%s%s\n", ok ? "ESCAPE" : "denied", op, path, ok ? "" : strerror(err),
           detail && detail[0] ? " " : "", detail ? detail : "");
    fflush(stdout);
}

static void probe_read(const char* path)
{
    char buffer[128] = {0};
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        report("read", path, 0, errno, "");
        return;
    }

    const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (count <= 0)
    {
        report("read", path, 0, errno, "(opened, empty)");
        return;
    }

    for (ssize_t i = 0; i < count; ++i)
    {
        if (buffer[i] == '\n')
        {
            buffer[i] = 0;
            break;
        }
    }

    report("read", path, 1, 0, buffer);
}

static void probe_write(const char* path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        report("write", path, 0, errno, "");
        return;
    }

    const ssize_t count = write(fd, "sogen-guest-was-here\n", 21);
    close(fd);
    report("write", path, count == 21, errno, "");
}

static void probe_unlink(const char* path)
{
    report("unlink", path, unlink(path) == 0, errno, "");
}

int main(void)
{
    probe_read("/Library/Caches/sogen-canary-read.txt");
    probe_read("/Applications/sogen-canary-read.txt");
    probe_read("/Library/Preferences/.GlobalPreferences.plist");
    probe_read("/etc/passwd");
    probe_read("/Users/Shared/sogen-canary-read.txt");
    probe_read("/../../../../etc/passwd");
    probe_read("/Library/../../../../etc/passwd");
    probe_read("/private/var/db/dslocal/nodes/Default/users/root.plist");

    probe_write("/Applications/sogen-escape-write.txt");
    probe_write("/Library/Caches/sogen-escape-write.txt");
    probe_write("/Users/Shared/sogen-escape-write.txt");
    probe_write("/System/Library/sogen-escape-write.txt");
    probe_write("/tmp/sogen-scratch-write.txt");

    probe_unlink("/Library/Caches/sogen-canary-delete.txt");
    probe_unlink("/Applications/sogen-canary-delete.txt");

    return 0;
}
