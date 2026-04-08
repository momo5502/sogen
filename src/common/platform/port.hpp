#pragma once

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)

#define LPC_REQUEST            1
#define LPC_REPLY              2
#define LPC_DATAGRAM           3
#define LPC_LOST_REPLY         4
#define LPC_PORT_CLOSED        5
#define LPC_CLIENT_DIED        6
#define LPC_EXCEPTION          7
#define LPC_DEBUG_EVENT        8
#define LPC_ERROR_EVENT        9
#define LPC_CONNECTION_REQUEST 10
#define LPC_NO_IMPERSONATE     0x4000
#define LPC_KERNELMODE_MESSAGE 0x8000

#define LpcpGetMessageType(x)  ((x)->u2.s2.Type & ~LPC_KERNELMODE_MESSAGE)

struct PORT_MESSAGE64
{
    union
    {
        struct
        {
            CSHORT DataLength;
            CSHORT TotalLength;
        } s1;

        ULONG Length;
    } u1;

    union
    {
        struct
        {
            CSHORT Type;
            CSHORT DataInfoOffset;
        } s2;

        ULONG ZeroInit;
    } u2;

    union
    {
        CLIENT_ID64 ClientId;
        double DoNotUseThisField;
    };

    ULONG MessageId;

    union
    {
        EmulatorTraits<Emu64>::SIZE_T ClientViewSize; // only valid for LPC_CONNECTION_REQUEST messages
        ULONG CallbackId;                             // only valid for LPC_REQUEST messages
    };
};

struct ALPC_MESSAGE_ATTRIBUTES
{
    ULONG AllocatedAttributes;
    ULONG ValidAttributes;
};

template <typename Traits>
struct PORT_DATA_ENTRY
{
    typename Traits::PVOID Base;
    ULONG Size;
};

template <typename Traits>
struct ALPC_SECURITY_ATTR
{
    ULONG Flags;
    typename Traits::PVOID SecurityQos;
    typename Traits::HANDLE ContextHandle;
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)
