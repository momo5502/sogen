#if defined(__arm64e__)
#define SOGEN_FIXTURE_MARKER "#0x5e"
#else
#define SOGEN_FIXTURE_MARKER "#0x2a"
#endif

__attribute__((naked)) void _start(void)
{
    __asm__ volatile("mov x0, " SOGEN_FIXTURE_MARKER "\n\t"
                     "mov x1, #0x1234\n\t"
                     "nop\n\t"
                     "nop\n\t"
                     "b .");
}
