#pragma once

#include "../std_include.hpp"

typedef LONG TDI_STATUS;
typedef PVOID CONNECTION_CONTEXT;

typedef struct _TDI_CONNECTION_INFORMATION
{
	LONG UserDataLength;
	PVOID UserData;
	LONG OptionsLength;
	PVOID Options;
	LONG RemoteAddressLength;
	PVOID RemoteAddress;
} TDI_CONNECTION_INFORMATION, *PTDI_CONNECTION_INFORMATION;

typedef struct _TDI_REQUEST
{
	union
	{
		HANDLE AddressHandle;
		CONNECTION_CONTEXT ConnectionContext;
		HANDLE ControlChannel;
	} Handle;

	PVOID RequestNotifyObject;
	PVOID RequestContext;
	TDI_STATUS TdiStatus;
} TDI_REQUEST, *PTDI_REQUEST;

typedef struct _TDI_REQUEST_SEND_DATAGRAM
{
	TDI_REQUEST Request;
	PTDI_CONNECTION_INFORMATION SendDatagramInformation;
} TDI_REQUEST_SEND_DATAGRAM, *PTDI_REQUEST_SEND_DATAGRAM;

typedef struct _AFD_SEND_DATAGRAM_INFO
{
	LPWSABUF BufferArray;
	ULONG BufferCount;
	ULONG AfdFlags;
	TDI_REQUEST_SEND_DATAGRAM TdiRequest;
	TDI_CONNECTION_INFORMATION TdiConnInfo;
} AFD_SEND_DATAGRAM_INFO, *PAFD_SEND_DATAGRAM_INFO;

#define _AFD_REQUEST(ioctl) \
                ((((ULONG)(ioctl)) >> 2) & 0x03FF)
#define _AFD_BASE(ioctl) \
                ((((ULONG)(ioctl)) >> 12) & 0xFFFFF)

#define FSCTL_AFD_BASE FILE_DEVICE_NETWORK

#define AFD_BIND                    0
#define AFD_CONNECT                 1
#define AFD_START_LISTEN            2
#define AFD_WAIT_FOR_LISTEN         3
#define AFD_ACCEPT                  4
#define AFD_RECEIVE                 5
#define AFD_RECEIVE_DATAGRAM        6
#define AFD_SEND                    7
#define AFD_SEND_DATAGRAM           8
#define AFD_POLL                    9
#define AFD_PARTIAL_DISCONNECT      10

#define AFD_GET_ADDRESS             11
#define AFD_QUERY_RECEIVE_INFO      12
#define AFD_QUERY_HANDLES           13
#define AFD_SET_INFORMATION         14
#define AFD_GET_CONTEXT_LENGTH      15
#define AFD_GET_CONTEXT             16
#define AFD_SET_CONTEXT             17

#define AFD_SET_CONNECT_DATA        18
#define AFD_SET_CONNECT_OPTIONS     19
#define AFD_SET_DISCONNECT_DATA     20
#define AFD_SET_DISCONNECT_OPTIONS  21

#define AFD_GET_CONNECT_DATA        22
#define AFD_GET_CONNECT_OPTIONS     23
#define AFD_GET_DISCONNECT_DATA     24
#define AFD_GET_DISCONNECT_OPTIONS  25

#define AFD_SIZE_CONNECT_DATA       26
#define AFD_SIZE_CONNECT_OPTIONS    27
#define AFD_SIZE_DISCONNECT_DATA    28
#define AFD_SIZE_DISCONNECT_OPTIONS 29

#define AFD_GET_INFORMATION         30
#define AFD_TRANSMIT_FILE           31
#define AFD_SUPER_ACCEPT            32

#define AFD_EVENT_SELECT            33
#define AFD_ENUM_NETWORK_EVENTS     34

#define AFD_DEFER_ACCEPT            35
#define AFD_WAIT_FOR_LISTEN_LIFO    36
#define AFD_SET_QOS                 37
#define AFD_GET_QOS                 38
#define AFD_NO_OPERATION            39
#define AFD_VALIDATE_GROUP          40
#define AFD_GET_UNACCEPTED_CONNECT_DATA 41
