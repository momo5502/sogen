# Minimal Linux x86-64 "Hello World" using raw syscalls
# Assemble: x86_64-elf-as -o hello.o hello.S
# Link:     x86_64-elf-ld -o hello hello.o

.global _start

.section .text

_start:
    # write(1, msg, 13)
    mov $1, %rax        # syscall: write
    mov $1, %rdi        # fd: stdout
    lea msg(%rip), %rsi # buf: message
    mov $13, %rdx       # count
    syscall

    # exit(0)
    mov $60, %rax       # syscall: exit
    xor %rdi, %rdi      # status: 0
    syscall

.section .rodata
msg:
    .ascii "Hello World!\n"
