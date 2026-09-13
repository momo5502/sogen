__attribute__((naked)) void _start(void)
{
    __asm__ volatile("adr x0, 2f\n\t"
                     "mov x1, #0\n\t"
                     "mov x2, #0\n\t"
                     "mov x16, #5\n\t"
                     "svc #0x80\n\t"
                     "mov x0, #1\n\t"
                     "adr x1, 1f\n\t"
                     "mov x2, #14\n\t"
                     "mov x16, #4\n\t"
                     "svc #0x80\n\t"
                     "mov x0, #0\n\t"
                     "mov x16, #1\n\t"
                     "svc #0x80\n\t"
                     "brk #0\n\t"
                     "1: .ascii \"Hello, sogen!\\n\"\n\t"
                     "2: .asciz \"/sogen-trace-fixture/no-such-file\"");
}
