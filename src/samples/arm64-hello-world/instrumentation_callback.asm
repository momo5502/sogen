    AREA |.text|, CODE, READONLY

    EXPORT instrumentation_callback

; Windows on ARM enters a process instrumentation callback on every return from a system call,
; with the original return address in X16. Count the returns through the pointer stored in
; TEB.NtTib.ArbitraryUserPointer, then resume.
instrumentation_callback
    ldr x17, [x18, #0x28]
    cbz x17, resume
    ldr x9, [x17]
    add x9, x9, #1
    str x9, [x17]
resume
    br x16

    END
