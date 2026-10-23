#include <errno.h>
#include <locale.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static int compare_ints(const void* left, const void* right)
{
    const int a = *(const int*)left;
    const int b = *(const int*)right;
    return (a > b) - (a < b);
}

static void at_exit_handler(void)
{
    printf("atexit handler ran\n");
}

static unsigned __stdcall thread_errno(void* argument)
{
    errno = *(int*)argument;
    return errno == *(int*)argument ? 0 : 1;
}

static int check_heap(void)
{
    char* buffer = malloc(16);
    if (!buffer)
    {
        return 0;
    }
    strcpy(buffer, "heap");
    char* grown = realloc(buffer, 1 << 20);
    if (!grown)
    {
        free(buffer);
        return 0;
    }
    const int ok = strcmp(grown, "heap") == 0;
    free(grown);

    int* zeroed = calloc(64, sizeof(int));
    const int zero = zeroed && zeroed[0] == 0 && zeroed[63] == 0;
    free(zeroed);
    return ok && zero;
}

static int check_file(void)
{
    const char* const name = "crt_static.tmp";
    FILE* file = fopen(name, "w");
    if (!file)
    {
        return 0;
    }
    fprintf(file, "line %d\n", 42);
    fclose(file);

    char line[32] = {0};
    file = fopen(name, "r");
    const int read = file && fgets(line, sizeof(line), file) != NULL;
    if (file)
    {
        fclose(file);
    }
    return read && strcmp(line, "line 42\n") == 0 && remove(name) == 0;
}

static int check_thread_errno(void)
{
    int value = ERANGE;
    errno = 0;
    const uintptr_t thread = _beginthreadex(NULL, 0, thread_errno, &value, 0, NULL);
    if (!thread)
    {
        return 0;
    }
    WaitForSingleObject((HANDLE)thread, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeThread((HANDLE)thread, &exit_code);
    CloseHandle((HANDLE)thread);
    return exit_code == 0 && errno == 0;
}

int main(int argc, char** argv)
{
    atexit(at_exit_handler);
    printf("static CRT: argc=%d argv[0]=%s\n", argc, argv[0]);

    char formatted[64];
    snprintf(formatted, sizeof(formatted), "%.3f %e %x", 3.14159, 1234.5, 0xBEEF);
    printf("format: %s\n", formatted);

    int numbers[] = {5, 3, 9, 1, 7};
    qsort(numbers, 5, sizeof(int), compare_ints);
    const int key = 7;
    const int* found = bsearch(&key, numbers, 5, sizeof(int), compare_ints);
    printf("sort: %d %d %d %d %d, found 7 at %d\n", numbers[0], numbers[1], numbers[2], numbers[3], numbers[4],
           found ? (int)(found - numbers) : -1);

    char* end = NULL;
    const double parsed = strtod("2.5e3xyz", &end);
    int scanned = 0;
    sscanf("value=17", "value=%d", &scanned);
    printf("parse: %.1f rest=%s scanned=%d\n", parsed, end, scanned);

    printf("locale: %s\n", setlocale(LC_ALL, "C"));
    printf("heap: %s\n", check_heap() ? "ok" : "fail");
    printf("file: %s\n", check_file() ? "ok" : "fail");
    printf("thread errno: %s\n", check_thread_errno() ? "ok" : "fail");
    printf("time: %s\n", time(NULL) > 0 ? "ok" : "fail");
    printf("environment: %s\n", getenv("SystemRoot") ? "ok" : "fail");
    return 0;
}
