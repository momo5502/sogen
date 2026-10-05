#pragma once

#include <windows.h>
#include <winternl.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace capture
{

    // The Windows SDK does not expose the ALPC declarations consistently.
    // PORT_MESSAGE and the message attribute structures below use the native x64 layouts.
    struct PORT_MESSAGE
    {
        union
        {
            struct
            {
                USHORT DataLength;
                USHORT TotalLength;
            } s1;

            ULONG Length;
        } u1;

        union
        {
            struct
            {
                USHORT Type;
                USHORT DataInfoOffset;
            } s2;

            ULONG ZeroInit;
        } u2;

        union
        {
            CLIENT_ID ClientId;
            double DoNotUseThisField;
        };

        ULONG MessageId;

        union
        {
            SIZE_T ClientViewSize;
            ULONG CallbackId;
        };
    };

    using PPORT_MESSAGE = PORT_MESSAGE*;
    static_assert(sizeof(PORT_MESSAGE) == 40);

    constexpr ULONG kAlpcMessageSecurityAttribute = 0x80000000;
    constexpr ULONG kAlpcMessageViewAttribute = 0x40000000;

    struct NativeAlpcPortAttributes;

    struct NativeAlpcMessageAttributes
    {
        ULONG AllocatedAttributes;
        ULONG ValidAttributes;
    };

    struct NativeAlpcDataViewAttr
    {
        ULONG Flags;
        HANDLE SectionHandle;
        PVOID ViewBase;
        SIZE_T ViewSize;
    };

    using PNativeAlpcPortAttributes = NativeAlpcPortAttributes*;
    using PNativeAlpcMessageAttributes = NativeAlpcMessageAttributes*;

    static_assert(sizeof(NativeAlpcMessageAttributes) == 8);
    static_assert(sizeof(NativeAlpcDataViewAttr) == 32);

    using NtAlpcConnectPortExFn = NTSTATUS(NTAPI*)(PHANDLE PortHandle, POBJECT_ATTRIBUTES ConnectionPortObjectAttributes,
                                                   POBJECT_ATTRIBUTES ClientPortObjectAttributes, PNativeAlpcPortAttributes PortAttributes,
                                                   ULONG Flags, PSECURITY_DESCRIPTOR ServerSecurityRequirements,
                                                   PPORT_MESSAGE ConnectionMessage, PSIZE_T BufferLength,
                                                   PNativeAlpcMessageAttributes OutMessageAttributes,
                                                   PNativeAlpcMessageAttributes InMessageAttributes, PLARGE_INTEGER Timeout);

    using NtAlpcSendWaitReceivePortFn = NTSTATUS(NTAPI*)(HANDLE PortHandle, ULONG Flags, PPORT_MESSAGE SendMessage,
                                                         PNativeAlpcMessageAttributes SendMessageAttributes, PPORT_MESSAGE ReceiveMessage,
                                                         PSIZE_T BufferLength, PNativeAlpcMessageAttributes ReceiveMessageAttributes,
                                                         PLARGE_INTEGER Timeout);

    using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                          PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);
    using NtDeviceIoControlFileFn = NTSTATUS(NTAPI*)(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                                     PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                                     ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
    using NtCloseFn = NTSTATUS(NTAPI*)(HANDLE Handle);

    extern NtOpenFileFn RealNtOpenFile;
    extern NtDeviceIoControlFileFn RealNtDeviceIoControlFile;
    extern NtCloseFn RealNtClose;

    NTSTATUS NTAPI HookNtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                  PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);
    NTSTATUS NTAPI HookNtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                             PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                             ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
    NTSTATUS NTAPI HookNtClose(HANDLE Handle);

    bool StartDeviceCapture(std::string& error);
    bool StopDeviceCapture(std::string& error);
    uint64_t DeviceIoCount() noexcept;

    extern NtAlpcConnectPortExFn RealNtAlpcConnectPortEx;
    extern NtAlpcSendWaitReceivePortFn RealNtAlpcSendWaitReceivePort;
    extern std::atomic<uintptr_t> SspiPortHandle;

    NTSTATUS NTAPI HookNtAlpcConnectPortEx(PHANDLE PortHandle, POBJECT_ATTRIBUTES ConnectionPortObjectAttributes,
                                           POBJECT_ATTRIBUTES ClientPortObjectAttributes, PNativeAlpcPortAttributes PortAttributes,
                                           ULONG Flags, PSECURITY_DESCRIPTOR ServerSecurityRequirements, PPORT_MESSAGE ConnectionMessage,
                                           PSIZE_T BufferLength, PNativeAlpcMessageAttributes OutMessageAttributes,
                                           PNativeAlpcMessageAttributes InMessageAttributes, PLARGE_INTEGER Timeout);

    NTSTATUS NTAPI HookNtAlpcSendWaitReceivePort(HANDLE PortHandle, ULONG Flags, PPORT_MESSAGE SendMessage,
                                                 PNativeAlpcMessageAttributes SendMessageAttributes, PPORT_MESSAGE ReceiveMessage,
                                                 PSIZE_T BufferLength, PNativeAlpcMessageAttributes ReceiveMessageAttributes,
                                                 PLARGE_INTEGER Timeout);

    // The directory must not already contain files; this prevents stale dumps from
    // being confused with the current run.
    bool PrepareOutputDirectory(const std::filesystem::path& directory, std::string& error);
    bool Flush(const std::filesystem::path& directory, std::string& error);
    uint64_t TransactionCount() noexcept;

    std::string SecurityStatusName(LONG status);
    std::string HexStatus(LONG status);
    std::string JsonEscape(const std::string& value);

} // namespace capture
