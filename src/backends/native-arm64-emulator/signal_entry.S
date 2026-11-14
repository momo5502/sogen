.text
.p2align 2
.global sogen_native_arm64_signal_entry
.hidden sogen_native_arm64_signal_entry
.type sogen_native_arm64_signal_entry, %function
sogen_native_arm64_signal_entry:
    adrp x9, sogen_native_arm64_host_x18
    ldr x18, [x9, :lo12:sogen_native_arm64_host_x18]
    adrp x9, sogen_native_arm64_host_tls
    ldr x9, [x9, :lo12:sogen_native_arm64_host_tls]
    msr tpidr_el0, x9
    b sogen_native_arm64_signal_dispatch
.size sogen_native_arm64_signal_entry, .-sogen_native_arm64_signal_entry
.section .note.GNU-stack,"",%progbits
