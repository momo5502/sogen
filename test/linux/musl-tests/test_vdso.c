#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

// We read the auxiliary vector from /proc/self/auxv to find AT_SYSINFO_EHDR
#define AT_NULL         0
#define AT_SYSINFO_EHDR 33

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

// Find AT_SYSINFO_EHDR from the auxiliary vector on the stack.
// We walk the environment variables to find the auxv that follows.
static uint64_t find_vdso_base(char** envp)
{
    // Walk past environment pointers to find the auxiliary vector
    char** ep = envp;
    while (*ep != NULL)
        ep++;
    ep++; // Skip the NULL terminator of envp

    // Now ep points to the auxiliary vector (pairs of uint64_t)
    uint64_t* auxv = (uint64_t*)ep;
    while (auxv[0] != AT_NULL)
    {
        if (auxv[0] == AT_SYSINFO_EHDR)
        {
            return auxv[1];
        }
        auxv += 2;
    }
    return 0;
}

static void test_vdso_present(char** envp)
{
    uint64_t base = find_vdso_base(envp);
    if (base == 0)
    {
        test_fail("vDSO present in auxv", "AT_SYSINFO_EHDR not found");
        return;
    }

    printf("  AT_SYSINFO_EHDR = 0x%lx\n", (unsigned long)base);

    // Verify it's a valid ELF header
    const unsigned char* ehdr = (const unsigned char*)base;
    if (ehdr[0] == 0x7f && ehdr[1] == 'E' && ehdr[2] == 'L' && ehdr[3] == 'F')
    {
        test_ok("vDSO present in auxv");
    }
    else
    {
        test_fail("vDSO present in auxv", "invalid ELF magic");
    }
}

static void test_vdso_elf_type(char** envp)
{
    uint64_t base = find_vdso_base(envp);
    if (base == 0)
    {
        test_fail("vDSO ELF type is ET_DYN", "no vDSO");
        return;
    }

    // e_type is at offset 16 (uint16_t)
    const uint16_t* e_type = (const uint16_t*)((const unsigned char*)base + 16);
    printf("  e_type = %u (expected 3 = ET_DYN)\n", *e_type);
    if (*e_type == 3) // ET_DYN
    {
        test_ok("vDSO ELF type is ET_DYN");
    }
    else
    {
        test_fail("vDSO ELF type is ET_DYN", "wrong type");
    }
}

static void test_vdso_machine(char** envp)
{
    uint64_t base = find_vdso_base(envp);
    if (base == 0)
    {
        test_fail("vDSO machine is EM_X86_64", "no vDSO");
        return;
    }

    // e_machine is at offset 18 (uint16_t)
    const uint16_t* e_machine = (const uint16_t*)((const unsigned char*)base + 18);
    printf("  e_machine = %u (expected 62 = EM_X86_64)\n", *e_machine);
    if (*e_machine == 62) // EM_X86_64
    {
        test_ok("vDSO machine is EM_X86_64");
    }
    else
    {
        test_fail("vDSO machine is EM_X86_64", "wrong machine");
    }
}

static void test_clock_gettime_works(void)
{
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));

    // This may or may not go through the vDSO depending on musl's init,
    // but either way it should work via the syscall fallback.
    int ret = clock_gettime(0 /* CLOCK_REALTIME */, &ts);
    if (ret != 0)
    {
        test_fail("clock_gettime(CLOCK_REALTIME)", "returned error");
        return;
    }

    printf("  clock_gettime: tv_sec=%ld, tv_nsec=%ld\n", (long)ts.tv_sec, (long)ts.tv_nsec);

    // Basic sanity: seconds should be > 0 (we're after 1970)
    if (ts.tv_sec > 0)
    {
        test_ok("clock_gettime(CLOCK_REALTIME)");
    }
    else
    {
        test_fail("clock_gettime(CLOCK_REALTIME)", "tv_sec is 0");
    }
}

static void test_gettimeofday_works(void)
{
    struct timeval tv;
    memset(&tv, 0, sizeof(tv));

    int ret = gettimeofday(&tv, NULL);
    if (ret != 0)
    {
        test_fail("gettimeofday", "returned error");
        return;
    }

    printf("  gettimeofday: tv_sec=%ld, tv_usec=%ld\n", (long)tv.tv_sec, (long)tv.tv_usec);

    if (tv.tv_sec > 0)
    {
        test_ok("gettimeofday");
    }
    else
    {
        test_fail("gettimeofday", "tv_sec is 0");
    }
}

static void test_time_works(void)
{
    time_t t = time(NULL);
    printf("  time: %ld\n", (long)t);

    if (t > 0)
    {
        test_ok("time()");
    }
    else
    {
        test_fail("time()", "returned 0 or negative");
    }
}

static void test_vdso_in_maps(void)
{
    // Check that /proc/self/maps shows [vdso]
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f)
    {
        test_fail("[vdso] in /proc/self/maps", "cannot open maps");
        return;
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (strstr(buf, "[vdso]") != NULL)
    {
        test_ok("[vdso] in /proc/self/maps");
    }
    else
    {
        test_fail("[vdso] in /proc/self/maps", "not found");
    }
}

int main(int argc, char** argv, char** envp)
{
    printf("=== vDSO emulation tests ===\n\n");

    test_vdso_present(envp);
    test_vdso_elf_type(envp);
    test_vdso_machine(envp);
    test_clock_gettime_works();
    test_gettimeofday_works();
    test_time_works();
    test_vdso_in_maps();

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
