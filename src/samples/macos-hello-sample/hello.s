.globl _start
_start:
    mov $0x2000004, %eax
    mov $1, %edi
    lea message(%rip), %rsi
    mov $message_length, %edx
    syscall

    mov $0x2000001, %eax
    xor %edi, %edi
    syscall

message:
    .ascii "Hello from a macOS guest!\n"
message_length = . - message
