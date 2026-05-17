#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int test_count = 0;
static int pass_count = 0;

static void test_ok(const char* name)
{
    test_count++;
    pass_count++;
    printf("[PASS] %s\n", name);
}

static void test_fail(const char* name, const char* reason)
{
    test_count++;
    printf("[FAIL] %s: %s\n", name, reason);
}

static void test_readlink_self_exe(void)
{
    char buf[256];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0)
    {
        test_fail("readlink(/proc/self/exe)", "readlink failed");
        return;
    }
    buf[len] = '\0';
    printf("  /proc/self/exe -> %s\n", buf);

    if (len > 0)
    {
        test_ok("readlink(/proc/self/exe)");
    }
    else
    {
        test_fail("readlink(/proc/self/exe)", "empty result");
    }
}

static void test_readlink_self_fd(void)
{
    char buf[256];
    ssize_t len = readlink("/proc/self/fd/1", buf, sizeof(buf) - 1);
    if (len < 0)
    {
        test_fail("readlink(/proc/self/fd/1)", "readlink failed");
        return;
    }
    buf[len] = '\0';
    printf("  /proc/self/fd/1 -> %s\n", buf);
    test_ok("readlink(/proc/self/fd/1)");
}

static void test_read_maps(void)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0)
    {
        test_fail("open(/proc/self/maps)", "open failed");
        return;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        test_fail("read(/proc/self/maps)", "no data");
        return;
    }
    buf[n] = '\0';

    printf("  /proc/self/maps (%zd bytes):\n", n);
    /* Print first few lines */
    int lines = 0;
    for (ssize_t i = 0; i < n && lines < 5; i++)
    {
        putchar(buf[i]);
        if (buf[i] == '\n')
            lines++;
    }
    if (lines >= 5)
        printf("  ...\n");

    /* Verify it contains expected markers */
    if (strstr(buf, "r-x") != NULL || strstr(buf, "r--") != NULL)
    {
        test_ok("read(/proc/self/maps)");
    }
    else
    {
        test_fail("read(/proc/self/maps)", "missing permission markers");
    }
}

static void test_read_cmdline(void)
{
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0)
    {
        test_fail("open(/proc/self/cmdline)", "open failed");
        return;
    }

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n <= 0)
    {
        test_fail("read(/proc/self/cmdline)", "no data");
        return;
    }

    printf("  /proc/self/cmdline args:");
    ssize_t i = 0;
    while (i < n)
    {
        printf(" '%s'", &buf[i]);
        i += strlen(&buf[i]) + 1;
    }
    printf("\n");

    test_ok("read(/proc/self/cmdline)");
}

static void test_read_status(void)
{
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
    {
        test_fail("open(/proc/self/status)", "open failed");
        return;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        test_fail("read(/proc/self/status)", "no data");
        return;
    }
    buf[n] = '\0';

    printf("  /proc/self/status (%zd bytes):\n", n);
    /* Print first few lines */
    int lines = 0;
    for (ssize_t i = 0; i < n && lines < 6; i++)
    {
        putchar(buf[i]);
        if (buf[i] == '\n')
            lines++;
    }
    if (lines >= 6)
        printf("  ...\n");

    /* Verify expected fields */
    if (strstr(buf, "Name:") != NULL && strstr(buf, "Pid:") != NULL && strstr(buf, "VmSize:") != NULL)
    {
        test_ok("read(/proc/self/status)");
    }
    else
    {
        test_fail("read(/proc/self/status)", "missing expected fields");
    }
}

static void test_read_osrelease(void)
{
    int fd = open("/proc/sys/kernel/osrelease", O_RDONLY);
    if (fd < 0)
    {
        test_fail("open(/proc/sys/kernel/osrelease)", "open failed");
        return;
    }

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0)
    {
        test_fail("read(/proc/sys/kernel/osrelease)", "no data");
        return;
    }
    buf[n] = '\0';

    /* Trim newline */
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';

    printf("  /proc/sys/kernel/osrelease: %s\n", buf);

    if (strstr(buf, "5.15") != NULL)
    {
        test_ok("read(/proc/sys/kernel/osrelease)");
    }
    else
    {
        test_fail("read(/proc/sys/kernel/osrelease)", "unexpected version");
    }
}

static void test_access_procfs(void)
{
    if (access("/proc/self/maps", 0 /* F_OK */) == 0)
    {
        test_ok("access(/proc/self/maps)");
    }
    else
    {
        test_fail("access(/proc/self/maps)", "access returned error");
    }
}

int main(int argc, char** argv)
{
    printf("=== procfs emulation tests ===\n");
    printf("argc=%d", argc);
    for (int i = 0; i < argc; i++)
        printf(" argv[%d]=%s", i, argv[i]);
    printf("\n\n");

    test_readlink_self_exe();
    test_readlink_self_fd();
    test_read_maps();
    test_read_cmdline();
    test_read_status();
    test_read_osrelease();
    test_access_procfs();

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);

    return (pass_count == test_count) ? 0 : 1;
}
