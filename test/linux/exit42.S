# Test: exit(42)
.global _start
.section .text
_start:
    mov $60, %rax       # syscall: exit
    mov $42, %rdi       # status: 42
    syscall
