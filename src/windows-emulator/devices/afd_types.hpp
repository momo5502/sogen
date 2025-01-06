#pragma once

#include "../std_include.hpp"

typedef LONG TDI_STATUS;

template <typename Traits>
struct TDI_CONNECTION_INFORMATION
{
    LONG UserDataLength;
    typename Traits::PVOID UserData;
    LONG OptionsLength;
    typename Traits::PVOID Options;
    LONG RemoteAddressLength;
    typename Traits::PVOID RemoteAddress;
};

template <typename Traits>
struct TDI_REQUEST
{
    union
    {
        typename Traits::HANDLE AddressHandle;
        EMULATOR_CAST(typename Traits::PVOID, CONNECTION_CONTEXT) ConnectionContext;
        typename Traits::HANDLE ControlChannel;
    } Handle;

    typename Traits::PVOID RequestNotifyObject;
    typename Traits::PVOID RequestContext;
    TDI_STATUS TdiStatus;
};

template <typename Traits>
struct TDI_REQUEST_SEND_DATAGRAM
{
    TDI_REQUEST<Traits> Request;
    EMULATOR_CAST(typename Traits::PVOID, PTDI_CONNECTION_INFORMATION) SendDatagramInformation;
};

template <typename Traits>
struct AFD_SEND_INFO
{
    EMULATOR_CAST(typename Traits::PVOID, LPWSABUF) BufferArray;
    ULONG BufferCount;
    ULONG AfdFlags;
    ULONG TdiFlags;
};

template <typename Traits>
struct AFD_SEND_DATAGRAM_INFO
{
    EMULATOR_CAST(typename Traits::PVOID, LPWSABUF) BufferArray;
    ULONG BufferCount;
    ULONG AfdFlags;
    TDI_REQUEST_SEND_DATAGRAM<Traits> TdiRequest;
    TDI_CONNECTION_INFORMATION<Traits> TdiConnInfo;
};

template <typename Traits>
struct AFD_RECV_INFO
{
    EMULATOR_CAST(typename Traits::PVOID, LPWSABUF) BufferArray;
    ULONG BufferCount;
    ULONG AfdFlags;
    ULONG TdiFlags;
};

template <typename Traits>
struct AFD_RECV_DATAGRAM_INFO
{
    EMULATOR_CAST(typename Traits::PVOID, LPWSABUF) BufferArray;
    ULONG BufferCount;
    ULONG AfdFlags;
    ULONG TdiFlags;
    typename Traits::PVOID Address;
    EMULATOR_CAST(typename Traits::PVOID, PULONG) AddressLength;
};

struct AFD_POLL_HANDLE_INFO64
{
    EmulatorTraits<Emu64>::HANDLE Handle;
    ULONG PollEvents;
    NTSTATUS Status;
};

struct AFD_POLL_INFO64
{
    LARGE_INTEGER Timeout;
    ULONG NumberOfHandles;
    BOOLEAN Unique;
    AFD_POLL_HANDLE_INFO64 Handles[1];
};

#define AFD_POLL_RECEIVE_BIT            0
#define AFD_POLL_RECEIVE                (1 << AFD_POLL_RECEIVE_BIT)
#define AFD_POLL_RECEIVE_EXPEDITED_BIT  1
#define AFD_POLL_RECEIVE_EXPEDITED      (1 << AFD_POLL_RECEIVE_EXPEDITED_BIT)
#define AFD_POLL_SEND_BIT               2
#define AFD_POLL_SEND                   (1 << AFD_POLL_SEND_BIT)
#define AFD_POLL_DISCONNECT_BIT         3
#define AFD_POLL_DISCONNECT             (1 << AFD_POLL_DISCONNECT_BIT)
#define AFD_POLL_ABORT_BIT              4
#define AFD_POLL_ABORT                  (1 << AFD_POLL_ABORT_BIT)
#define AFD_POLL_LOCAL_CLOSE_BIT        5
#define AFD_POLL_LOCAL_CLOSE            (1 << AFD_POLL_LOCAL_CLOSE_BIT)
#define AFD_POLL_CONNECT_BIT            6
#define AFD_POLL_CONNECT                (1 << AFD_POLL_CONNECT_BIT)
#define AFD_POLL_ACCEPT_BIT             7
#define AFD_POLL_ACCEPT                 (1 << AFD_POLL_ACCEPT_BIT)
#define AFD_POLL_CONNECT_FAIL_BIT       8
#define AFD_POLL_CONNECT_FAIL           (1 << AFD_POLL_CONNECT_FAIL_BIT)
#define AFD_POLL_QOS_BIT                9
#define AFD_POLL_QOS                    (1 << AFD_POLL_QOS_BIT)
#define AFD_POLL_GROUP_QOS_BIT          10
#define AFD_POLL_GROUP_QOS              (1 << AFD_POLL_GROUP_QOS_BIT)

#define AFD_NUM_POLL_EVENTS             11
#define AFD_POLL_ALL                    ((1 << AFD_NUM_POLL_EVENTS) - 1)

#define _AFD_REQUEST(ioctl)             ((((ULONG)(ioctl)) >> 2) & 0x03FF)
#define _AFD_BASE(ioctl)                ((((ULONG)(ioctl)) >> 12) & 0xFFFFF)

#define FSCTL_AFD_BASE                  FILE_DEVICE_NETWORK

#define AFD_BIND                        0
#define AFD_CONNECT                     1
#define AFD_START_LISTEN                2
#define AFD_WAIT_FOR_LISTEN             3
#define AFD_ACCEPT                      4
#define AFD_RECEIVE                     5
#define AFD_RECEIVE_DATAGRAM            6
#define AFD_SEND                        7
#define AFD_SEND_DATAGRAM               8
#define AFD_POLL                        9
#define AFD_PARTIAL_DISCONNECT          10

#define AFD_GET_ADDRESS                 11
#define AFD_QUERY_RECEIVE_INFO          12
#define AFD_QUERY_HANDLES               13
#define AFD_SET_INFORMATION             14
#define AFD_GET_CONTEXT_LENGTH          15
#define AFD_GET_CONTEXT                 16
#define AFD_SET_CONTEXT                 17

#define AFD_SET_CONNECT_DATA            18
#define AFD_SET_CONNECT_OPTIONS         19
#define AFD_SET_DISCONNECT_DATA         20
#define AFD_SET_DISCONNECT_OPTIONS      21

#define AFD_GET_CONNECT_DATA            22
#define AFD_GET_CONNECT_OPTIONS         23
#define AFD_GET_DISCONNECT_DATA         24
#define AFD_GET_DISCONNECT_OPTIONS      25

#define AFD_SIZE_CONNECT_DATA           26
#define AFD_SIZE_CONNECT_OPTIONS        27
#define AFD_SIZE_DISCONNECT_DATA        28
#define AFD_SIZE_DISCONNECT_OPTIONS     29

#define AFD_GET_INFORMATION             30
#define AFD_TRANSMIT_FILE               31
#define AFD_SUPER_ACCEPT                32

#define AFD_EVENT_SELECT                33
#define AFD_ENUM_NETWORK_EVENTS         34

#define AFD_DEFER_ACCEPT                35
#define AFD_WAIT_FOR_LISTEN_LIFO        36
#define AFD_SET_QOS                     37
#define AFD_GET_QOS                     38
#define AFD_NO_OPERATION                39
#define AFD_VALIDATE_GROUP              40
#define AFD_GET_UNACCEPTED_CONNECT_DATA 41
