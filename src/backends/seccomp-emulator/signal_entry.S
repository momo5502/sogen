.text
.p2align 2
.global sogen_seccomp_signal_entry
.hidden sogen_seccomp_signal_entry
.type sogen_seccomp_signal_entry, %function
sogen_seccomp_signal_entry:
    adrp x9, sogen_seccomp_host_x18
    ldr x18, [x9, :lo12:sogen_seccomp_host_x18]
    adrp x9, sogen_seccomp_host_tls
    ldr x9, [x9, :lo12:sogen_seccomp_host_tls]
    msr tpidr_el0, x9
    b sogen_seccomp_signal_dispatch
.size sogen_seccomp_signal_entry, .-sogen_seccomp_signal_entry
.section .note.GNU-stack,"",%progbits
