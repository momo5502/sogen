#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test dynamic linking with various libc features
int main(void)
{
    // Test printf with formatting
    printf("Testing dynamic linking features:\n");

    // Test malloc/free
    char *buf = malloc(256);
    if (!buf) { printf("FAIL: malloc\n"); return 1; }
    snprintf(buf, 256, "heap allocated: %p", (void*)buf);
    printf("  %s\n", buf);
    free(buf);
    printf("  malloc/free: OK\n");

    // Test string functions
    char dst[64];
    strcpy(dst, "Hello");
    strcat(dst, ", World!");
    printf("  strcat: %s (len=%zu)\n", dst, strlen(dst));

    // Test strtol
    long val = strtol("12345", NULL, 10);
    printf("  strtol: %ld\n", val);
    if (val != 12345) { printf("FAIL: strtol\n"); return 1; }

    // Test getenv
    const char *path = getenv("PATH");
    printf("  PATH=%s\n", path ? path : "(null)");

    // Test atoi
    int x = atoi("42");
    printf("  atoi: %d\n", x);
    if (x != 42) { printf("FAIL: atoi\n"); return 1; }

    printf("\nAll dynamic linking tests passed!\n");
    return 0;
}
