# Test multiple syscalls: arch_prctl, brk, uname, write, exit
.global _start

.section .text

_start:
    # arch_prctl(ARCH_SET_FS, tls_area)
    mov $158, %rax          # syscall: arch_prctl
    mov $0x1002, %rdi       # ARCH_SET_FS
    lea tls_area(%rip), %rsi
    syscall

    # brk(0) - get current brk
    mov $12, %rax           # syscall: brk
    xor %rdi, %rdi
    syscall

    # uname(buf) - get system info
    mov $63, %rax           # syscall: uname
    lea uname_buf(%rip), %rdi
    syscall

    # Write the sysname from uname (first 65 bytes = sysname field)
    # First write a label
    mov $1, %rax
    mov $1, %rdi
    lea label_msg(%rip), %rsi
    mov $9, %rdx
    syscall

    # Write the sysname
    mov $1, %rax
    mov $1, %rdi
    lea uname_buf(%rip), %rsi
    mov $5, %rdx            # "Linux" = 5 chars
    syscall

    # Newline
    mov $1, %rax
    mov $1, %rdi
    lea newline(%rip), %rsi
    mov $1, %rdx
    syscall

    # Write success message
    mov $1, %rax
    mov $1, %rdi
    lea ok_msg(%rip), %rsi
    mov $14, %rdx
    syscall

    # exit(0)
    mov $60, %rax
    xor %rdi, %rdi
    syscall

.section .data
tls_area:
    .space 256

.section .bss
uname_buf:
    .space 390              # struct utsname is 65*6=390 bytes

.section .rodata
label_msg:
    .ascii "sysname: "
ok_msg:
    .ascii "All tests OK!\n"
newline:
    .ascii "\n"
