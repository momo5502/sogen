#include <asm/unistd.h>

.text
.p2align 2
.global sogen_seccomp_signal_entry
.hidden sogen_seccomp_signal_entry
.type sogen_seccomp_signal_entry, %function
sogen_seccomp_signal_entry:
    stp x0, x1, [sp, #-32]!
    stp x2, x8, [sp, #16]
    // TLS is not usable until the receiving thread is identified.
    mov x8, #__NR_gettid
    svc #0
    adrp x9, sogen_seccomp_owner_tid
    add x9, x9, :lo12:sogen_seccomp_owner_tid
    ldar w9, [x9]
    cmp w0, w9
    ldp x2, x8, [sp, #16]
    ldp x0, x1, [sp], #32
    b.eq .Lowner_signal
    b sogen_seccomp_host_signal_dispatch
.Lowner_signal:
    adrp x9, sogen_seccomp_host_x18
    ldr x18, [x9, :lo12:sogen_seccomp_host_x18]
    adrp x9, sogen_seccomp_host_tls
    ldr x9, [x9, :lo12:sogen_seccomp_host_tls]
    msr tpidr_el0, x9
    b sogen_seccomp_signal_dispatch
.size sogen_seccomp_signal_entry, .-sogen_seccomp_signal_entry
.section .note.GNU-stack,"",%progbits
