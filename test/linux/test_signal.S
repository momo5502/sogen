# Test signal delivery: install SIGSEGV handler, trigger fault, recover
# Tests: rt_sigaction, signal frame build, sigreturn
.global _start

.section .text

# Signal handler - receives signum in %rdi
sigsegv_handler:
    # Write "SIGSEGV caught!\n"
    mov $1, %rax            # sys_write
    mov $1, %rdi            # fd = stdout
    lea caught_msg(%rip), %rsi
    mov $16, %rdx           # length
    syscall

    # Modify the RIP in the ucontext to skip past the faulting instruction
    # RDX = pointer to ucontext_t
    # ucontext_t layout:
    #   +0:   uc_flags (8)
    #   +8:   uc_link (8)
    #   +16:  uc_stack (24 = ss_sp(8) + ss_flags(4) + pad(4) + ss_size(8))
    #   +40:  uc_mcontext (sigcontext):
    #         +40: r8, +48: r9, +56: r10, +64: r11
    #         +72: r12, +80: r13, +88: r14, +96: r15
    #         +104: rdi, +112: rsi, +120: rbp, +128: rbx
    #         +136: rdx, +144: rax, +152: rcx
    #         +160: rsp, +168: rip
    # So RIP is at ucontext + 40 + 128 = ucontext + 168

    # RDX was set by kernel to &ucontext, but we clobbered it with sys_write.
    # We need to get it from the stack frame. The handler was called with:
    #   RDI=signum, RSI=&siginfo, RDX=&ucontext
    # But we clobbered those. We need to access the frame differently.
    #
    # Actually, the pretcode is at [RSP] when the handler starts.
    # After our function prologue... let's just use RSP to find the frame.
    #
    # When handler is entered: RSP points to pretcode in the signal frame.
    # The frame layout is: [pretcode(8)][siginfo(128)][ucontext(...)]
    # So ucontext is at RSP + 8 + 128 = RSP + 136
    #
    # But we did a syscall which doesn't change RSP. So RSP still points
    # to the frame (pretcode). Let's compute ucontext addr.
    mov %rsp, %rax
    add $136, %rax          # rax = &ucontext (8 pretcode + 128 siginfo)

    # RIP is at ucontext + 168 (offset 40 for mcontext start + 128 for rip field)
    # sigcontext offsets: r8(0), r9(8), r10(16), r11(24), r12(32), r13(40), r14(48), r15(56)
    #                     rdi(64), rsi(72), rbp(80), rbx(88), rdx(96), rax(104), rcx(112)
    #                     rsp(120), rip(128)
    # mcontext starts at ucontext + 40
    # So RIP = ucontext + 40 + 128 = ucontext + 168
    mov 168(%rax), %rcx     # rcx = saved RIP (faulting instruction)
    add $2, %rcx            # skip past the faulting "mov (%rax), %rax" (2 bytes: 48 8b 00 = 3 bytes for REX+MOV, but mov (%rax),%rcx without REX is 2 bytes 8b 08... let's use the right size)
    # Actually: the faulting instruction is "movq (%rax), %rcx" = 48 8b 08 (3 bytes with REX prefix)
    # Let's just skip 3 bytes
    add $1, %rcx            # total +3 to skip the 3-byte faulting instruction
    mov %rcx, 168(%rax)     # update saved RIP to instruction after the fault

    # Also set a known value in saved RAX to prove we modified context
    # saved rax is at ucontext + 40 + 104 = ucontext + 144
    movq $0xDEAD, 144(%rax) # set saved rax = 0xDEAD

    # Return from handler (will go to sigreturn trampoline via pretcode)
    ret

_start:
    # Write "Installing handler...\n"
    mov $1, %rax
    mov $1, %rdi
    lea install_msg(%rip), %rsi
    mov $22, %rdx
    syscall

    # rt_sigaction(SIGSEGV=11, &act, NULL, 8)
    # struct kernel_sigaction {
    #   uint64_t sa_handler;   // +0
    #   uint64_t sa_flags;     // +8
    #   uint64_t sa_restorer;  // +16
    #   uint64_t sa_mask;      // +24
    # }
    lea sigaction_buf(%rip), %rsi
    lea sigsegv_handler(%rip), %rax
    mov %rax, 0(%rsi)       # sa_handler = sigsegv_handler
    movq $0x4, 8(%rsi)      # sa_flags = SA_SIGINFO (0x4)
    movq $0, 16(%rsi)       # sa_restorer = 0
    movq $0, 24(%rsi)       # sa_mask = 0

    mov $13, %rax           # syscall: rt_sigaction
    mov $11, %rdi           # signum = SIGSEGV
                            # rsi already = &act
    xor %rdx, %rdx          # oldact = NULL
    mov $8, %r10            # sigsetsize = 8
    syscall

    # Check return value
    test %rax, %rax
    jnz sigaction_failed

    # Write "Triggering SIGSEGV...\n"
    mov $1, %rax
    mov $1, %rdi
    lea trigger_msg(%rip), %rsi
    mov $22, %rdx
    syscall

    # Trigger SIGSEGV by reading from NULL
    xor %rax, %rax          # rax = 0 (NULL)
    movq (%rax), %rcx       # FAULT! This should trigger SIGSEGV
    # This is 3 bytes: 48 8b 08 (REX.W MOV r64, r/m64)

    # If signal handler ran and fixed RIP, we land here with rax = 0xDEAD
    cmp $0xDEAD, %rax
    jne handler_failed

    # Write "Signal recovery OK!\n"
    mov $1, %rax
    mov $1, %rdi
    lea ok_msg(%rip), %rsi
    mov $20, %rdx
    syscall

    # exit(0)
    mov $60, %rax
    xor %rdi, %rdi
    syscall

sigaction_failed:
    mov $1, %rax
    mov $1, %rdi
    lea fail_sigaction_msg(%rip), %rsi
    mov $22, %rdx
    syscall
    mov $60, %rax
    mov $1, %rdi
    syscall

handler_failed:
    mov $1, %rax
    mov $1, %rdi
    lea fail_handler_msg(%rip), %rsi
    mov $27, %rdx
    syscall
    mov $60, %rax
    mov $2, %rdi
    syscall

.section .data
sigaction_buf:
    .space 32               # struct kernel_sigaction (4 * 8 bytes)

.section .rodata
install_msg:
    .ascii "Installing handler...\n"
trigger_msg:
    .ascii "Triggering SIGSEGV...\n"
caught_msg:
    .ascii "SIGSEGV caught!\n"
ok_msg:
    .ascii "Signal recovery OK!\n"
fail_sigaction_msg:
    .ascii "sigaction failed!\n"
fail_handler_msg:
    .ascii "Handler did not run!\n"
