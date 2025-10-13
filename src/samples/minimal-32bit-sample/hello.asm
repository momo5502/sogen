.386
.model flat, stdcall
option casemap:none

includelib ntdll.lib

extrn _imp__NtCreateFile@44:DWORD        ; 11 * 4 = 44
extrn _imp__NtWriteFile@36:DWORD         ; 9  * 4 = 36
extrn _imp__NtTerminateProcess@8:DWORD   ; 2  * 4 = 8

STATUS_SUCCESS          equ 0
GENERIC_WRITE          equ 40000000h
SYNCHRONIZE            equ 00100000h
FILE_ATTRIBUTE_NORMAL  equ 00000080h
FILE_SHARE_READ        equ 00000001h
FILE_SHARE_WRITE       equ 00000002h
FILE_OPEN              equ 00000001h
FILE_SYNCHRONOUS_IO_NONALERT equ 00000020h
OBJ_CASE_INSENSITIVE   equ 00000040h

UNICODE_STRING STRUCT
    Length1         WORD    ?
    MaximumLength   WORD    ?
    Buffer          DWORD   ?
UNICODE_STRING ENDS

OBJECT_ATTRIBUTES STRUCT
    Length1                 DWORD   ?
    RootDirectory          DWORD   ?
    ObjectName             DWORD   ?
    Attributes             DWORD   ?
    SecurityDescriptor     DWORD   ?
    SecurityQualityOfService DWORD ?
OBJECT_ATTRIBUTES ENDS

IO_STATUS_BLOCK STRUCT
    Status      DWORD   ?
    Information DWORD   ?
IO_STATUS_BLOCK ENDS

.data
    ConsoleName     dw 005Ch, 003Fh, 003Fh, 005Ch, 0043h, 004Fh, 004Eh, 004Fh, 0055h, 0054h, 0024h, 0000h
    
    HelloMsg        db 'hello', 0Dh, 0Ah
    HelloMsgLen     equ $ - HelloMsg
    
    DeviceName      UNICODE_STRING <22, 24, OFFSET ConsoleName>
    
    ObjAttr         OBJECT_ATTRIBUTES <18h, 0, OFFSET DeviceName, OBJ_CASE_INSENSITIVE, 0, 0>
    
    IoStatus        IO_STATUS_BLOCK <0, 0>
    
    hConsole        DWORD   0

.code
start:
    push    0                               ; EaLength
    push    0                               ; EaBuffer
    push    FILE_SYNCHRONOUS_IO_NONALERT    ; CreateOptions
    push    FILE_OPEN                       ; CreateDisposition
    push    FILE_SHARE_READ or FILE_SHARE_WRITE ; ShareAccess
    push    FILE_ATTRIBUTE_NORMAL           ; FileAttributes
    push    0                               ; AllocationSize
    push    OFFSET IoStatus                 ; IoStatusBlock
    push    OFFSET ObjAttr                  ; ObjectAttributes
    push    GENERIC_WRITE or SYNCHRONIZE    ; DesiredAccess
    push    OFFSET hConsole                 ; FileHandle
    call    dword ptr [_imp__NtCreateFile@44]
    
    test    eax, eax
    jnz     exit_error
    
    push    0                   ; Key
    push    0                   ; ByteOffset
    push    HelloMsgLen         ; Length
    push    OFFSET HelloMsg     ; Buffer
    push    OFFSET IoStatus     ; IoStatusBlock
    push    0                   ; ApcContext
    push    0                   ; ApcRoutine
    push    0                   ; Event
    push    hConsole            ; FileHandle
    call    dword ptr [_imp__NtWriteFile@36]
    
    push    0                   ; ExitStatus
    push    -1                  ; ProcessHandle
    call    dword ptr [_imp__NtTerminateProcess@8]
    
exit_error:
    push    1                   ; ExitStatus
    push    -1                  ; ProcessHandle
    call    dword ptr [_imp__NtTerminateProcess@8]

end start